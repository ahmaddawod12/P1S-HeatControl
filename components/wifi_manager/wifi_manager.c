#include "wifi_manager.h"
#include "wifi_engine.h"
#include "wifi_ap_shutdown.h"
#include "app_runtime.h"
#include "service_operation.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#define WORKER_STACK 4096
#define RESPONSE_GRACE_MS 250U
#define RSSI_REFRESH_MS 5000U
#define NAME_SETTLE_MS 3000U
static const char *TAG="wifi";
static bool s_stack_ready, s_storage_ready, s_mdns_service_ready;
static atomic_bool s_mdns_ready;
static esp_netif_t *s_sta_netif;
static _Atomic(SemaphoreHandle_t) s_lock;
static _Atomic(TaskHandle_t) s_worker;
static wifi_engine_t s_engine;
static wifi_manager_status_t s_status;
static bool s_pending;
static uint64_t s_apply_at;
static uint64_t s_rssi_refresh_at;
static wifi_credentials_t s_requested;
static char s_saved_name[DEVICE_NAME_LEN];
static bool s_name_dirty, s_name_network_connected;
static bool s_name_preflight;
static char s_candidate_hostname[DEVICE_HOSTNAME_LEN];
static uint64_t s_name_check_at, s_discovery_retry_at;
static esp_err_t start_mdns(void);
typedef struct {
    bool pending, connected;
    char ssid[WIFI_MANAGER_SSID_LEN], ip[WIFI_MANAGER_IP_LEN];
    int8_t rssi;
    wifi_result_t result;
} network_event_t;
static network_event_t s_event;
static atomic_uint s_disconnect_epoch;
static atomic_bool s_disconnect_requested;
static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time()/1000); }

static void queue_event(const network_event_t *event)
{
    xSemaphoreTake(s_lock,portMAX_DELAY);
    s_event=*event;
    if (!event->connected) atomic_fetch_add(&s_disconnect_epoch,1);
    xSemaphoreGive(s_lock);
    if (s_worker) xTaskNotifyGive(s_worker);
}
static void got_ip_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg; (void)base;
    if (id==IP_EVENT_STA_LOST_IP) {
        const network_event_t event={.pending=true,.result=WIFI_RESULT_CONNECTING};
        queue_event(&event);
        ESP_LOGW(TAG,"station IP lost; starting network recovery");
        return;
    }
    if (id!=IP_EVENT_STA_GOT_IP || data==NULL) return;
    const ip_event_got_ip_t *got=data;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap)!=ESP_OK) return;
    network_event_t event={.pending=true,.connected=true,.rssi=ap.rssi};
    memcpy(event.ssid,ap.ssid,sizeof(event.ssid)); event.ssid[sizeof(event.ssid)-1]=0;
    snprintf(event.ip,sizeof(event.ip),IPSTR,IP2STR(&got->ip_info.ip));
    queue_event(&event);
    ESP_LOGI(TAG,"connected as %s (rssi %d)",event.ip,event.rssi);
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg; (void)base;
    if (id==WIFI_EVENT_STA_CONNECTED) {
        /* An idle radio may not emit our requested disconnect. Never let
         * that intent mask a failure of the new association before DHCP. */
        atomic_store(&s_disconnect_requested,false);
        return;
    }
    if (id!=WIFI_EVENT_STA_DISCONNECTED) return;
    const wifi_event_sta_disconnected_t *disconnected=data;
    const bool requested=atomic_exchange(&s_disconnect_requested,false);
    if (requested && disconnected->reason==WIFI_REASON_ASSOC_LEAVE) return;
    wifi_result_t result=WIFI_RESULT_DRIVER_FAILED;
    switch (disconnected->reason) {
    case WIFI_REASON_NO_AP_FOUND: result=WIFI_RESULT_NO_NETWORK; break;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: result=WIFI_RESULT_AUTH_FAILED; break;
    case WIFI_REASON_ASSOC_LEAVE: result=WIFI_RESULT_CONNECTING; break;
    default: break;
    }
    const network_event_t event={.pending=true,.result=result};
    queue_event(&event);
    ESP_LOGW(TAG,"station disconnected (reason %u); control continues",(unsigned)disconnected->reason);
}
static esp_err_t driver_ap(bool enabled,void *ctx)
{
    (void)ctx;
    const esp_err_t err=esp_wifi_set_mode(enabled?WIFI_MODE_APSTA:WIFI_MODE_STA);
    if (err==ESP_OK) ESP_LOGI(TAG,"setup hotspot %s at %s",enabled?"started":"stopped",WIFI_MANAGER_AP_IP);
    return err;
}
static uint32_t disconnect_epoch(void *ctx)
{ (void)ctx; return atomic_load(&s_disconnect_epoch); }
static bool disconnect_pending(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    const bool pending=s_event.pending && !s_event.connected;
    xSemaphoreGive(s_lock);
    return pending;
}
static esp_err_t native_ap(bool enabled,void *ctx)
{
    if (enabled) return driver_ap(true,ctx);
    const wifi_ap_shutdown_ops_t ops={.set_ap=driver_ap,.disconnect_epoch=disconnect_epoch,
        .disconnect_pending=disconnect_pending};
    return wifi_ap_shutdown(&ops);
}
static esp_err_t native_save(const wifi_credentials_t *credentials,void *ctx)
{
    (void)ctx;
    const esp_err_t err=device_config_save_wifi(credentials);
    if (err==ESP_OK) {
        memcpy(s_saved_name,credentials->controller_name,sizeof(s_saved_name));
        s_name_dirty=true;
    }
    return err;
}
static esp_err_t select_name(const char *name,uint64_t now)
{
    char hostname[DEVICE_HOSTNAME_LEN];
    esp_err_t err=device_hostname(name,hostname);
    if (err!=ESP_OK) return err;
    char candidate[DEVICE_NAME_LEN]={0};
    strcpy(candidate,name[0]?name:DEVICE_DEFAULT_NAME);
    memcpy(s_engine.status.controller_name,candidate,sizeof(candidate));
    strcpy(s_candidate_hostname,hostname);
    s_engine.status.hostname[0]=0;
    s_name_preflight=true;
    s_engine.status.name_pending=true;
    s_name_check_at=now+NAME_SETTLE_MS;
    s_discovery_retry_at=0;
    return ESP_OK;
}
static esp_err_t native_connect(const wifi_credentials_t *credentials,void *ctx)
{
    (void)ctx;
    if (s_name_dirty) {
        const esp_err_t err=select_name(credentials->controller_name,now_ms());
        if (err!=ESP_OK) return err;
        memcpy(s_engine.status.requested_name,credentials->controller_name,sizeof(credentials->controller_name));
        s_name_dirty=false;
    }
    wifi_config_t current;
    const bool same=esp_wifi_get_config(WIFI_IF_STA,&current)==ESP_OK &&
        memcmp(current.sta.ssid,credentials->ssid,sizeof(current.sta.ssid))==0 &&
        memcmp(current.sta.password,credentials->password,sizeof(current.sta.password))==0;
    if (!same) {
        atomic_store(&s_disconnect_requested,true);
        if (esp_wifi_disconnect()!=ESP_OK) atomic_store(&s_disconnect_requested,false);
        /* Discard a DHCP notification captured before this reconfiguration. */
        xSemaphoreTake(s_lock,portMAX_DELAY);
        if (s_event.connected) s_event.pending=false;
        xSemaphoreGive(s_lock);
        wifi_config_t config={0};
        memcpy(config.sta.ssid,credentials->ssid,sizeof(config.sta.ssid));
        memcpy(config.sta.password,credentials->password,sizeof(config.sta.password));
        const esp_err_t err=esp_wifi_set_config(WIFI_IF_STA,&config);
        if (err!=ESP_OK) return err;
    } else {
        wifi_ap_record_t ap; esp_netif_ip_info_t ip;
        if (esp_wifi_sta_get_ap_info(&ap)==ESP_OK && esp_netif_get_ip_info(s_sta_netif,&ip)==ESP_OK && ip.ip.addr) {
            network_event_t event={.pending=true,.connected=true,.rssi=ap.rssi};
            memcpy(event.ssid,ap.ssid,sizeof(event.ssid)); event.ssid[sizeof(event.ssid)-1]=0;
            snprintf(event.ip,sizeof(event.ip),IPSTR,IP2STR(&ip.ip)); queue_event(&event);
            return ESP_OK;
        }
    }
    return esp_wifi_connect();
}
static void publish(void)
{
    static char last_ip[WIFI_MANAGER_IP_LEN]; static int8_t last_rssi;
    wifi_manager_status_t status=s_engine.status;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    status.applying=s_pending || service_operation_current()==SERVICE_OPERATION_WIFI;
    status.can_save=s_storage_ready && !status.applying && service_operation_current()==SERVICE_OPERATION_NONE;
    s_status=status;
    xSemaphoreGive(s_lock);
    if (strcmp(last_ip,status.ip_addr)!=0 || last_rssi!=status.rssi) {
        strcpy(last_ip,status.ip_addr); last_rssi=status.rssi;
        app_runtime_set_network_status(status.rssi,status.ip_addr);
    }
}
static void refresh_rssi(uint64_t now)
{
    if (!s_engine.status.connected || now<s_rssi_refresh_at) return;
    /* Bound failed queries too; RSSI is advisory and must not drive recovery. */
    s_rssi_refresh_at=now+RSSI_REFRESH_MS;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    const unsigned epoch=atomic_load(&s_disconnect_epoch);
    const bool pending=s_event.pending;
    xSemaphoreGive(s_lock);
    if (pending) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap)!=ESP_OK) return;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    /* A new association/IP event or a disconnect invalidates this sample.
     * Never route a signal update through connected(): it renews AP grace. */
    if (!s_event.pending && epoch==atomic_load(&s_disconnect_epoch) &&
        strncmp((const char *)ap.ssid,s_engine.status.ssid,sizeof(ap.ssid))==0)
        s_engine.status.rssi=ap.rssi;
    xSemaphoreGive(s_lock);
}
static void recover_name_setup(void)
{
    if (!s_engine.status.ap_active) s_engine.ap_recovery_needed=true;
}
static esp_err_t next_name(uint64_t now)
{
    char next[DEVICE_NAME_LEN];
    esp_err_t err=device_name_next(s_engine.status.controller_name,next);
    if (err==ESP_OK) err=select_name(next,now);
    recover_name_setup();
    return err;
}
static esp_err_t preflight_name(void)
{
    /* Publish the pending state before changing mDNS or waiting for replies.
     * Readers must never replace this snapshot with the internal identity. */
    publish();
    /* Keep the desired name unclaimed while checking its existing owner.
     * This temporary identity is unique to the station IP on this link and
     * is never saved or shown as the controller's final address. */
    char temporary[DEVICE_HOSTNAME_LEN];
    /* Even the shortest IPv4 makes this longer than every valid user name. */
    snprintf(temporary,sizeof(temporary),"p1s-discovery-check-%s",s_engine.status.ip_addr);
    for (char *p=temporary; *p; ++p) if (*p=='.') *p='-';
    esp_err_t err=mdns_hostname_set(temporary);
    if (err!=ESP_OK) return err;
    const unsigned epoch=atomic_load(&s_disconnect_epoch);
    mdns_result_t *results=NULL;
    /* This bounded wait is in the connectivity task; HTTP and heat control
     * keep running. Check one candidate per iteration, never a retry loop. */
    err=mdns_query(s_candidate_hostname,NULL,NULL,MDNS_TYPE_A,1000,8,&results);
    bool occupied=false;
    for (const mdns_result_t *r=results; r; r=r->next)
        if (r->esp_netif==s_sta_netif) occupied=true;
    mdns_query_results_free(results);
    if (err!=ESP_OK) return err;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    const bool stale=s_pending || s_event.pending || epoch!=atomic_load(&s_disconnect_epoch);
    xSemaphoreGive(s_lock);
    if (stale) return ESP_ERR_NOT_ALLOWED;
    if (occupied) return next_name(now_ms());
    err=mdns_hostname_set(s_candidate_hostname);
    if (err==ESP_OK) err=mdns_instance_name_set(s_candidate_hostname);
    if (err==ESP_OK && !s_mdns_service_ready) {
        err=mdns_service_add(NULL,"_http","_tcp",80,NULL,0);
        if (err==ESP_OK) s_mdns_service_ready=true;
    }
    if (err!=ESP_OK) return err;
    strcpy(s_engine.status.hostname,s_candidate_hostname);
    s_name_preflight=false;
    s_engine.status.result=WIFI_RESULT_CONNECTED;
    s_name_check_at=now_ms()+NAME_SETTLE_MS;
    return ESP_OK;
}
static void refresh_name(uint64_t now)
{
    if (now<s_discovery_retry_at) return;
    if (start_mdns()!=ESP_OK) {
        s_engine.status.name_pending=true;
        s_engine.status.result=WIFI_RESULT_DRIVER_FAILED;
        s_discovery_retry_at=now+1000;
        recover_name_setup();
        return;
    }
    const bool connected=s_engine.status.connected;
    if (connected!=s_name_network_connected) {
        s_name_network_connected=connected;
        s_engine.status.name_pending=true;
        s_name_check_at=now+NAME_SETTLE_MS;
        s_name_preflight=true;
        s_engine.status.hostname[0]=0;
    }
    if (!connected || !s_engine.status.configured) return;
    if (s_name_preflight) {
        const esp_err_t err=preflight_name();
        if (err!=ESP_OK && err!=ESP_ERR_NOT_ALLOWED) {
            s_engine.status.result=WIFI_RESULT_DRIVER_FAILED;
            s_discovery_retry_at=now_ms()+1000;
            recover_name_setup();
        }
        return;
    }
    char actual[DEVICE_HOSTNAME_LEN];
    if (mdns_hostname_get(actual)!=ESP_OK) {
        s_engine.status.name_pending=true;
        s_name_check_at=now+NAME_SETTLE_MS;
        return;
    }
    if (strcmp(actual,s_engine.status.hostname)!=0) {
        /* mDNS probes and negotiates simultaneous claims. A library rename
         * means this candidate lost; follow the user's p1s-1, p1s-2 sequence. */
        s_engine.status.name_pending=true;
        if (next_name(now)!=ESP_OK || preflight_name()!=ESP_OK)
            s_engine.status.result=WIFI_RESULT_DRIVER_FAILED;
        return;
    }
    if (!connected || !s_engine.status.configured || !s_engine.status.name_pending || now<s_name_check_at) return;
    if (!service_operation_begin(SERVICE_OPERATION_WIFI)) return;
    esp_err_t err=ESP_OK;
    if (strcmp(s_saved_name,s_engine.status.controller_name)!=0) {
        wifi_credentials_t saved={0};
        memcpy(saved.ssid,s_engine.status.ssid,sizeof(saved.ssid));
        memcpy(saved.password,s_engine.status.password,sizeof(saved.password));
        memcpy(saved.controller_name,s_engine.status.controller_name,sizeof(saved.controller_name));
        err=device_config_save_wifi(&saved);
        if (err==ESP_OK) memcpy(s_saved_name,saved.controller_name,sizeof(s_saved_name));
    }
    service_operation_end(SERVICE_OPERATION_WIFI);
    if (err!=ESP_OK) {
        s_engine.status.result=WIFI_RESULT_SAVE_FAILED;
        s_name_check_at=now+1000;
        recover_name_setup();
        return;
    }
    s_engine.status.name_pending=false;
    s_engine.status.result=WIFI_RESULT_CONNECTED;
    /* Leave time to read the final address before the setup hotspot closes. */
    if (s_engine.status.ap_active) s_engine.close_ap_at=now+10000;
    ESP_LOGI(TAG,"controller address confirmed: http://%s.local",s_engine.status.hostname);
}
static void worker(void *arg)
{
    (void)arg;
    const wifi_engine_ops_t ops={.save=native_save,.set_ap=native_ap,.connect=native_connect};
    for (;;) {
        const uint64_t now=now_ms();
        network_event_t event; wifi_credentials_t requested;
        xSemaphoreTake(s_lock,portMAX_DELAY);
        event=s_event; s_event.pending=false;
        const bool apply=s_pending && now>=s_apply_at;
        if (apply) { requested=s_requested; s_pending=false; }
        xSemaphoreGive(s_lock);
        if (event.pending) {
            if (event.connected) {
                wifi_engine_connected(&s_engine,event.ssid,event.ip,event.rssi,now);
                if (s_engine.status.connected && strcmp(event.ssid,s_engine.status.ssid)==0) {
                    s_rssi_refresh_at=now+RSSI_REFRESH_MS;
                    if (s_mdns_ready) {
                        s_engine.status.name_pending=true;
                        s_name_check_at=now+NAME_SETTLE_MS;
                        s_name_preflight=true;
                        s_engine.status.hostname[0]=0;
                    }
                }
            }
            else wifi_engine_disconnected(&s_engine,event.result,now);
        }
        if (apply) {
            (void)wifi_engine_apply(&s_engine,&requested,now,&ops);
            service_operation_end(SERVICE_OPERATION_WIFI);
        }
        refresh_name(now);
        wifi_engine_tick(&s_engine,now,!s_engine.status.name_pending && service_operation_current()==SERVICE_OPERATION_NONE,&ops);
        refresh_rssi(now);
        publish();
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));
    }
}
static esp_err_t init_stack(void)
{
    /* Completed stages own long-lived SDK resources. A later failure must
     * resume here rather than recreate default netifs/handlers or reinitialize
     * an already initialized radio. Keep configuration stages too, so a failed
     * start is retried without changing the radio mode underneath it. */
    enum {
        STACK_NONE, STACK_NVS, STACK_NETIF, STACK_EVENT_LOOP, STACK_STA,
        STACK_AP, STACK_RADIO, STACK_STORAGE, STACK_WIFI_HANDLER,
        STACK_IP_HANDLER, STACK_AP_MODE, STACK_AP_CONFIG, STACK_STA_MODE
    };
    static unsigned stage=STACK_NONE;
    if (s_stack_ready) return ESP_OK;
    esp_err_t err;
    if (stage<STACK_NVS) {
        err=nvs_flash_init();
        if (err==ESP_ERR_NVS_NO_FREE_PAGES || err==ESP_ERR_NVS_NEW_VERSION_FOUND) {
            err=nvs_flash_erase(); if (err==ESP_OK) err=nvs_flash_init();
        }
        if (err!=ESP_OK) return err;
        stage=STACK_NVS;
    }
    if (stage<STACK_NETIF) {
        err=esp_netif_init(); if (err!=ESP_OK) return err;
        stage=STACK_NETIF;
    }
    if (stage<STACK_EVENT_LOOP) {
        err=esp_event_loop_create_default();
        if (err!=ESP_OK && err!=ESP_ERR_INVALID_STATE) return err;
        stage=STACK_EVENT_LOOP;
    }
    if (stage<STACK_STA) {
        s_sta_netif=esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) return ESP_ERR_NO_MEM;
        stage=STACK_STA;
    }
    if (stage<STACK_AP) {
        if (!esp_netif_create_default_wifi_ap()) return ESP_ERR_NO_MEM;
        stage=STACK_AP;
    }
    if (stage<STACK_RADIO) {
        const wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
        err=esp_wifi_init(&init); if (err!=ESP_OK) return err;
        stage=STACK_RADIO;
    }
    if (stage<STACK_STORAGE) {
        err=esp_wifi_set_storage(WIFI_STORAGE_RAM); if (err!=ESP_OK) return err;
        stage=STACK_STORAGE;
    }
    if (stage<STACK_WIFI_HANDLER) {
        err=esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL,NULL);
        if (err!=ESP_OK) return err;
        stage=STACK_WIFI_HANDLER;
    }
    if (stage<STACK_IP_HANDLER) {
        err=esp_event_handler_instance_register(IP_EVENT,ESP_EVENT_ANY_ID,got_ip_event,NULL,NULL);
        if (err!=ESP_OK) return err;
        stage=STACK_IP_HANDLER;
    }
    /* Configure AP before starting the radio so no default SSID is exposed. */
    if (stage<STACK_AP_MODE) {
        err=esp_wifi_set_mode(WIFI_MODE_APSTA); if (err!=ESP_OK) return err;
        stage=STACK_AP_MODE;
    }
    if (stage<STACK_AP_CONFIG) {
        wifi_config_t ap={0};
        memcpy(ap.ap.ssid,WIFI_MANAGER_AP_SSID,strlen(WIFI_MANAGER_AP_SSID));
        ap.ap.ssid_len=strlen(WIFI_MANAGER_AP_SSID); ap.ap.channel=1;
        ap.ap.authmode=WIFI_AUTH_OPEN; ap.ap.max_connection=4;
        err=esp_wifi_set_config(WIFI_IF_AP,&ap); if (err!=ESP_OK) return err;
        stage=STACK_AP_CONFIG;
    }
    if (stage<STACK_STA_MODE) {
        err=esp_wifi_set_mode(WIFI_MODE_STA); if (err!=ESP_OK) return err;
        stage=STACK_STA_MODE;
    }
    err=esp_wifi_start(); if (err!=ESP_OK) return err;
    s_stack_ready=true; return ESP_OK;
}
static esp_err_t start_mdns(void)
{
    if (s_mdns_ready) return ESP_OK;
    if (!s_candidate_hostname[0]) {
        memcpy(s_engine.status.requested_name,s_engine.status.controller_name,sizeof(s_engine.status.requested_name));
        const esp_err_t name_err=select_name(s_engine.status.controller_name,now_ms());
        if (name_err!=ESP_OK) return name_err;
    }
    const esp_err_t err=mdns_init();
    if (err==ESP_OK) s_mdns_ready=true;
    return err;
}
esp_err_t wifi_manager_start(void)
{
    if (s_worker) return ESP_OK;
    if (!s_lock) s_lock=xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    wifi_credentials_t credentials;
    esp_err_t storage=device_config_init();
    bool configured=false;
    if (storage==ESP_OK) {
        storage=device_config_load_wifi(&credentials);
        if (storage==ESP_ERR_NOT_FOUND) {
            storage=ESP_OK;
            if (wifi_credentials_init(&credentials,CONFIG_P1S_WIFI_SSID,CONFIG_P1S_WIFI_PASSWORD)==ESP_OK) {
                storage=device_config_seed_wifi(&credentials);
                configured=storage==ESP_OK;
            }
        } else configured=storage==ESP_OK;
    }
    xSemaphoreTake(s_lock,portMAX_DELAY); s_storage_ready=storage==ESP_OK; xSemaphoreGive(s_lock);
    if (storage!=ESP_OK) (void)app_runtime_report_startup_fault(HEAT_FAULT_NVS);
    const esp_err_t err=init_stack(); if (err!=ESP_OK) return err;
    wifi_engine_init(&s_engine,configured?&credentials:NULL,storage,now_ms());
    memset(s_saved_name,0,sizeof(s_saved_name));
    if (configured) memcpy(s_saved_name,credentials.controller_name,sizeof(s_saved_name));
    memcpy(s_engine.status.requested_name,s_saved_name,sizeof(s_saved_name));
    s_name_network_connected=false; s_name_dirty=false;
    const esp_err_t name_err=select_name(s_saved_name,now_ms());
    if (name_err!=ESP_OK) return name_err;
    xSemaphoreTake(s_lock,portMAX_DELAY); s_status=s_engine.status; xSemaphoreGive(s_lock);
    TaskHandle_t created;
    if (xTaskCreate(worker,"wifi_worker",WORKER_STACK,NULL,4,&created)!=pdPASS) return ESP_ERR_NO_MEM;
    atomic_store(&s_worker,created);
    return ESP_OK;
}
void wifi_manager_get_status(wifi_manager_status_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out,0,sizeof(*out)); return; }
    xSemaphoreTake(s_lock,portMAX_DELAY); *out=s_status;
    out->can_save=s_worker && s_storage_ready && !s_pending && service_operation_current()==SERVICE_OPERATION_NONE;
    xSemaphoreGive(s_lock);
}
esp_err_t wifi_manager_request_credentials(const wifi_credentials_t *credentials)
{
    if (!credentials || !wifi_credentials_are_valid_n(credentials->ssid,sizeof(credentials->ssid),
        credentials->password,sizeof(credentials->password)) ||
        !device_name_valid_n(credentials->controller_name,sizeof(credentials->controller_name))) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    const bool ready=s_worker && s_storage_ready;
    xSemaphoreGive(s_lock);
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (!service_operation_begin(SERVICE_OPERATION_WIFI)) return ESP_ERR_NOT_ALLOWED;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    s_requested=*credentials; s_pending=true; s_apply_at=now_ms()+RESPONSE_GRACE_MS;
    s_status.applying=true; s_status.can_save=false;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_worker); return ESP_OK;
}
