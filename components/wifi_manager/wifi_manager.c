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
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#ifdef CONFIG_P1S_FIREBASE_PUBLIC_RELEASE
#include "bootloader_random.h"
#include "esp_random.h"
static char s_setup_passphrase[DEVICE_SETUP_PASSPHRASE_BYTES];

static esp_err_t load_setup_passphrase(void)
{
    esp_err_t err = device_config_load_setup_passphrase(s_setup_passphrase);
    if (err != ESP_ERR_NOT_FOUND) return err;

    uint8_t entropy[(DEVICE_SETUP_PASSPHRASE_BYTES - 1U) / 2U];
    static const char hex[] = "0123456789abcdef";
    /* Wi-Fi has not started, so use the hardware entropy source temporarily. */
    bootloader_random_enable();
    esp_fill_random(entropy, sizeof(entropy));
    bootloader_random_disable();
    for (size_t i = 0; i < sizeof(entropy); ++i) {
        s_setup_passphrase[2U * i] = hex[entropy[i] >> 4];
        s_setup_passphrase[2U * i + 1U] = hex[entropy[i] & 0x0fU];
    }
    s_setup_passphrase[DEVICE_SETUP_PASSPHRASE_BYTES - 1U] = '\0';
    return device_config_save_setup_passphrase(s_setup_passphrase);
}
#endif

#define WORKER_STACK 4096
#define RESPONSE_GRACE_MS 250U
#define RSSI_REFRESH_MS 5000U
static const char *TAG="wifi";
static bool s_stack_ready, s_storage_ready;
static char s_board_id[WIFI_MANAGER_BOARD_ID_LEN];
static esp_netif_t *s_sta_netif;
static _Atomic(SemaphoreHandle_t) s_lock;
static _Atomic(TaskHandle_t) s_worker;
static atomic_bool s_paired;
static _Atomic(wifi_setup_portal_fn) s_portal_start, s_portal_stop;
static bool s_portal_running;
static wifi_engine_t s_engine;
static wifi_manager_status_t s_status;
static bool s_pending;
static uint64_t s_apply_at;
static uint64_t s_rssi_refresh_at;
static wifi_credentials_t s_requested;
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
    if (!enabled && s_portal_running) {
        const wifi_setup_portal_fn stop = atomic_load(&s_portal_stop);
        if (!stop || stop() != ESP_OK) return ESP_ERR_NOT_ALLOWED;
        s_portal_running = false;
    }
    const esp_err_t err=esp_wifi_set_mode(enabled?WIFI_MODE_APSTA:WIFI_MODE_STA);
    if (err==ESP_OK) {
        *(bool *)ctx = enabled;
        ESP_LOGI(TAG, "setup hotspot " WIFI_MANAGER_AP_SSID_PREFIX "-%s %s at %s",
                 s_board_id, enabled ? "started" : "stopped", WIFI_MANAGER_AP_IP);
#ifdef CONFIG_P1S_FIREBASE_PUBLIC_RELEASE
        if (enabled) ESP_LOGI(TAG, "setup passphrase: %s", s_setup_passphrase);
#endif
    }
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
static esp_err_t native_ap(bool enabled, bool *active, void *ctx)
{
    (void)ctx;
    if (enabled) return driver_ap(true, active);
    const wifi_ap_shutdown_ops_t ops={.set_ap=driver_ap,.disconnect_epoch=disconnect_epoch,
        .disconnect_pending=disconnect_pending,.ctx=active};
    return wifi_ap_shutdown(&ops);
}
static esp_err_t native_save(const wifi_credentials_t *credentials,void *ctx)
{
    wifi_save_result_t *result = ctx;
    const esp_err_t err=device_config_save_wifi(credentials);
    *result = err == ESP_OK ? WIFI_SAVE_SAVED : WIFI_SAVE_FAILED;
    return err;
}
static esp_err_t native_connect(const wifi_credentials_t *credentials,void *ctx)
{
    (void)ctx;
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
    status.applying=s_pending;
    status.save_id = s_status.save_id;
    status.save_result = s_status.save_result;
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
static void worker(void *arg)
{
    (void)arg;
    wifi_save_result_t save_result;
    const wifi_engine_ops_t ops = {
        .save = native_save, .set_ap = native_ap, .connect = native_connect, .ctx = &save_result,
    };
    uint64_t portal_retry_at = 0;
    for (;;) {
        const uint64_t now=now_ms();
        wifi_credentials_t requested;
        xSemaphoreTake(s_lock,portMAX_DELAY);
        const network_event_t event=s_event;
        s_event.pending=false;
        if (event.pending) {
            if (event.connected) {
                wifi_engine_connected(&s_engine,event.ssid,event.ip,event.rssi);
                if (s_engine.status.connected && strcmp(event.ssid,s_engine.status.ssid)==0)
                    s_rssi_refresh_at=now+RSSI_REFRESH_MS;
            } else {
                wifi_engine_disconnected(&s_engine,event.result,now);
            }
            s_status.connected=s_engine.status.connected;
        }
        const bool apply=s_pending && now>=s_apply_at;
        if (apply) { requested=s_requested; s_pending=false; }
        const bool save_queued = s_pending;
        xSemaphoreGive(s_lock);
        if (apply) {
            save_result = WIFI_SAVE_FAILED;
            (void)wifi_engine_apply(&s_engine,&requested,now,&ops);
            xSemaphoreTake(s_lock,portMAX_DELAY);
            s_status.save_result = s_engine.status.result == WIFI_RESULT_AP_FAILED
                ? WIFI_SAVE_AP_FAILED : save_result;
            xSemaphoreGive(s_lock);
            service_operation_end(SERVICE_OPERATION_WIFI);
        }
        const bool paired = atomic_load(&s_paired);
        wifi_engine_tick(&s_engine,now,paired,&ops);
        refresh_rssi(now);
        const bool setup = save_queued || wifi_setup_mode(s_engine.status.connected, paired) != WIFI_SETUP_NONE;
        const wifi_setup_portal_fn start = atomic_load(&s_portal_start);
        if (setup && s_engine.status.ap_active && !s_portal_running && start && now >= portal_retry_at) {
            s_portal_running = start() == ESP_OK;
            portal_retry_at = now + 1000U;
        }
        s_engine.status.setup_portal_ready = setup && s_engine.status.ap_active && s_portal_running;
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
        uint8_t mac[6];
        err = esp_wifi_get_mac(WIFI_IF_STA, mac);
        if (err != ESP_OK) return err;
        snprintf(s_board_id, sizeof(s_board_id), "%02X%02X%02X%02X%02X%02X",
                 (unsigned) mac[0], (unsigned) mac[1], (unsigned) mac[2],
                 (unsigned) mac[3], (unsigned) mac[4], (unsigned) mac[5]);
        _Static_assert(sizeof(WIFI_MANAGER_AP_SSID_PREFIX) + WIFI_MANAGER_BOARD_ID_LEN <= sizeof(ap.ap.ssid),
                       "Setup hotspot SSID must fit");
        ap.ap.ssid_len = (uint8_t) snprintf((char *) ap.ap.ssid, sizeof(ap.ap.ssid),
                                            WIFI_MANAGER_AP_SSID_PREFIX "-%s", s_board_id);
        ap.ap.channel=1;
        ap.ap.authmode=WIFI_AUTH_OPEN; ap.ap.max_connection=4;
#ifdef CONFIG_P1S_FIREBASE_PUBLIC_RELEASE
        _Static_assert(sizeof(s_setup_passphrase) <= sizeof(ap.ap.password), "Setup passphrase must fit");
        memcpy(ap.ap.password, s_setup_passphrase, sizeof(s_setup_passphrase));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
#elif defined(CONFIG_P1S_FIREBASE_ENABLED)
        static const char passphrase[] = CONFIG_P1S_FIREBASE_SETUP_PASSPHRASE;
        if (sizeof(passphrase) < 9U || sizeof(passphrase) > sizeof(ap.ap.password)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(ap.ap.password, passphrase, sizeof(passphrase));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
#endif
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
esp_err_t wifi_manager_start(void)
{
    if (s_worker) return ESP_OK;
    if (!s_lock) s_lock=xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    wifi_credentials_t credentials;
    esp_err_t storage=device_config_init();
#ifdef CONFIG_P1S_FIREBASE_PUBLIC_RELEASE
    if (storage == ESP_OK) storage = load_setup_passphrase();
    if (storage != ESP_OK) {
        (void)app_runtime_report_startup_fault(HEAT_FAULT_NVS);
        return storage;
    }
#endif
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
    memcpy(s_engine.status.board_id, s_board_id, sizeof(s_board_id));
    xSemaphoreTake(s_lock,portMAX_DELAY); s_status=s_engine.status; xSemaphoreGive(s_lock);
    TaskHandle_t created;
    if (xTaskCreate(worker,"wifi_worker",WORKER_STACK,NULL,4,&created)!=pdPASS) return ESP_ERR_NO_MEM;
    atomic_store(&s_worker,created);
    return ESP_OK;
}
static wifi_setup_mode_t setup_mode_locked(void)
{
    /* An accepted save can still replace the network that just got an IP. */
    if (s_status.applying) return WIFI_SETUP_NETWORK;
    bool connected = s_status.connected;
    if (s_event.pending && (!s_event.connected ||
        (s_status.configured && strcmp(s_event.ssid, s_status.ssid) == 0))) {
        connected = s_event.connected;
    }
    return wifi_setup_mode(connected, atomic_load(&s_paired));
}

void wifi_manager_get_status(wifi_manager_status_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out,0,sizeof(*out)); return; }
    xSemaphoreTake(s_lock,portMAX_DELAY);
    *out=s_status;
    out->setup_mode=setup_mode_locked();
    out->can_save=out->setup_mode!=WIFI_SETUP_NONE && s_worker && s_storage_ready &&
        !out->applying && service_operation_current()==SERVICE_OPERATION_NONE;
    xSemaphoreGive(s_lock);
}

bool wifi_manager_lock_connection(uint32_t *generation)
{
    if (!generation || !s_lock) return false;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    const bool ready = s_status.connected && !s_status.applying &&
        !(s_event.pending && !s_event.connected);
    if (ready) {
        *generation = atomic_load(&s_disconnect_epoch);
        return true;
    }
    xSemaphoreGive(s_lock);
    return false;
}

void wifi_manager_unlock_connection(void)
{
    xSemaphoreGive(s_lock);
}

bool wifi_manager_connection_generation(uint32_t *generation)
{
    if (!wifi_manager_lock_connection(generation)) return false;
    wifi_manager_unlock_connection();
    return true;
}

esp_err_t wifi_manager_request_credentials(const wifi_credentials_t *credentials, uint32_t *save_id)
{
    if (!credentials || !wifi_credentials_are_valid_n(credentials->ssid,sizeof(credentials->ssid),
        credentials->password,sizeof(credentials->password))) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (!service_operation_begin(SERVICE_OPERATION_WIFI)) return ESP_ERR_NOT_ALLOWED;

    xSemaphoreTake(s_lock,portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_worker || !s_storage_ready) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_status.applying || setup_mode_locked() == WIFI_SETUP_NONE) {
        err = ESP_ERR_NOT_ALLOWED;
    } else {
        s_requested=*credentials;
        memcpy(s_requested.controller_name,s_status.controller_name,sizeof(s_requested.controller_name));
        s_pending=true;
        s_apply_at=now_ms()+RESPONSE_GRACE_MS;
        s_status.applying=true;
        atomic_fetch_add(&s_disconnect_epoch,1);
        if (++s_status.save_id == 0) ++s_status.save_id;
        s_status.save_result = WIFI_SAVE_PENDING;
        if (save_id) *save_id = s_status.save_id;
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) xTaskNotifyGive(s_worker);
    else service_operation_end(SERVICE_OPERATION_WIFI);
    return err;
}

void wifi_manager_set_setup_portal(wifi_setup_portal_fn start, wifi_setup_portal_fn stop)
{
    atomic_store(&s_portal_stop, stop);
    atomic_store(&s_portal_start, start);
    const TaskHandle_t worker_task = atomic_load(&s_worker);
    if (worker_task) xTaskNotifyGive(worker_task);
}

void wifi_manager_set_paired(bool paired)
{
    if (atomic_exchange(&s_paired, paired) == paired) return;
    const TaskHandle_t worker_task = atomic_load(&s_worker);
    if (worker_task) xTaskNotifyGive(worker_task);
}
