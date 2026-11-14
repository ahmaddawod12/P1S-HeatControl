#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

/* Exercise the native callbacks and worker, including their private event
 * dispatch, with only the radio/RTOS boundaries replaced. */
#include "../../components/wifi_manager/wifi_manager.c"

static jmp_buf s_iteration_done;
static uint64_t s_test_now;
static unsigned s_connect_calls;
static wifi_config_t s_radio_config;
static bool s_associated;
static esp_err_t s_disconnect_result;
static unsigned s_ap_info_calls, s_network_updates, s_ap_stops;
static int8_t s_radio_rssi, s_published_rssi;
static char s_published_ip[WIFI_MANAGER_IP_LEN];
static esp_err_t s_ap_info_result;
static const char *s_reported_ssid;
static bool s_disconnect_during_query;
static const wifi_credentials_t k_old = {"old network", "old-password"};
static const wifi_credentials_t k_next = {"new network", "new-password"};

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, TickType_t wait)
{ (void)wait; assert(lock); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t lock)
{ assert(lock); return pdTRUE; }
BaseType_t xTaskNotifyGive(TaskHandle_t task) { assert(task); return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{ (void)clear; (void)wait; longjmp(s_iteration_done, 1); }
int64_t esp_timer_get_time(void) { return (int64_t)s_test_now * 1000; }
void app_runtime_set_network_status(int8_t rssi, const char *ip)
{ ++s_network_updates; s_published_rssi = rssi; strcpy(s_published_ip, ip); }
esp_err_t device_config_save_wifi(const device_wifi_credentials_t *credentials)
{ assert(credentials); return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{ if (mode == WIFI_MODE_STA) ++s_ap_stops; return ESP_OK; }
esp_err_t esp_wifi_get_config(int interface, wifi_config_t *config)
{ assert(interface == WIFI_IF_STA); *config = s_radio_config; return ESP_OK; }
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config)
{ assert(interface == WIFI_IF_STA); s_radio_config = *config; return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{
    ++s_ap_info_calls;
    if (s_ap_info_result != ESP_OK) return s_ap_info_result;
    if (!s_associated) return ESP_FAIL;
    memset(ap, 0, sizeof(*ap));
    memcpy(ap->ssid, s_radio_config.sta.ssid, sizeof(s_radio_config.sta.ssid));
    if (s_reported_ssid) strcpy((char *)ap->ssid, s_reported_ssid);
    ap->rssi = s_radio_rssi;
    if (s_disconnect_during_query) {
        s_disconnect_during_query = false;
        s_associated = false;
        const wifi_event_sta_disconnected_t event = {.reason = WIFI_REASON_NO_AP_FOUND};
        wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, (void *)&event);
    }
    return ESP_OK;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip)
{ (void)netif; memset(ip, 0, sizeof(*ip)); return ESP_OK; }
esp_err_t esp_wifi_connect(void) { ++s_connect_calls; return ESP_OK; }
esp_err_t esp_wifi_disconnect(void)
{ if (s_disconnect_result == ESP_OK) s_associated = false; return s_disconnect_result; }

static void run_worker_at(uint64_t now)
{
    s_test_now = now;
    /* The notification wait ends exactly one real worker iteration. */
    if (setjmp(s_iteration_done) == 0) worker(NULL);
}
static void associated(void)
{
    s_associated = true;
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
}
static void disconnected(uint8_t reason)
{
    s_associated = false;
    const wifi_event_sta_disconnected_t event = {.reason = reason};
    wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, (void *)&event);
}
static void reset_fixture(bool radio_configured)
{
    s_lock = xSemaphoreCreateMutex();
    s_worker = (TaskHandle_t)1;
    s_storage_ready = true;
    s_pending = false;
    s_event = (network_event_t){0};
    atomic_store(&s_disconnect_epoch, 0);
    service_operation_end(SERVICE_OPERATION_WIFI);
    s_test_now = 0;
    s_connect_calls = 0;
    s_ap_info_calls = s_network_updates = s_ap_stops = 0;
    s_radio_rssi = -40;
    s_published_rssi = 0;
    s_published_ip[0] = 0;
    s_ap_info_result = ESP_OK;
    s_reported_ssid = NULL;
    s_disconnect_during_query = false;
    s_radio_config = (wifi_config_t){0};
    if (radio_configured) {
        memcpy(s_radio_config.sta.ssid, k_old.ssid, sizeof(s_radio_config.sta.ssid));
        memcpy(s_radio_config.sta.password, k_old.password, sizeof(s_radio_config.sta.password));
    }
    s_disconnect_result = ESP_OK;
    /* Also establish a fresh association boundary for callback state. */
    associated();
    s_associated = false;
    wifi_engine_init(&s_engine, &k_old, ESP_OK, 0);
}
static void assert_retry_after_disconnect(uint64_t at, unsigned attempts)
{
    disconnected(WIFI_REASON_ASSOC_LEAVE);
    run_worker_at(at);
    run_worker_at(at + 999);
    assert(s_connect_calls == attempts);
    run_worker_at(at + 1000);
    assert(s_connect_calls == attempts + 1);
}
static void router_disconnect_before_dhcp_retries(void)
{
    reset_fixture(true);
    run_worker_at(0);
    assert(s_connect_calls == 1);
    associated();
    assert_retry_after_disconnect(500, 1);
}
static void association_clears_an_unreported_local_disconnect(void)
{
    reset_fixture(false);
    /* Disconnecting an idle radio may produce no notification. The new
     * association must not leave that request masking router failures. */
    run_worker_at(0);
    associated();
    assert_retry_after_disconnect(500, 1);
}
static void credential_change_consumes_only_its_own_disconnect(void)
{
    reset_fixture(true);
    run_worker_at(0);
    associated();
    assert(wifi_manager_request_credentials(&k_next) == ESP_OK);
    run_worker_at(250);
    assert(s_connect_calls == 2);
    disconnected(WIFI_REASON_ASSOC_LEAVE);
    run_worker_at(350);
    run_worker_at(5000);
    assert(s_connect_calls == 2);
    /* Once the local notification is consumed, another reason-8 failure
     * before GOT_IP must still arm the ordinary backoff. */
    assert_retry_after_disconnect(6000, 2);
}
static void failed_disconnect_does_not_mask_router_failure(void)
{
    reset_fixture(false);
    s_disconnect_result = ESP_FAIL;
    run_worker_at(0);
    assert_retry_after_disconnect(500, 1);
}
static void lease_loss_opens_fallback_and_reacquires(void)
{
    reset_fixture(true);
    run_worker_at(0);
    associated();
    ip_event_got_ip_t acquired = {.ip_info.ip.addr = 0x1600000a};
    got_ip_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &acquired);
    run_worker_at(1000);
    assert(s_engine.status.connected && !s_engine.status.ap_active);
    /* Keep the station associated: only its DHCP lease has disappeared.
     * The event ID must be handled before interpreting a GOT_IP payload. */
    got_ip_event(NULL, IP_EVENT, IP_EVENT_STA_LOST_IP, &acquired);
    run_worker_at(1500);
    assert(!s_engine.status.connected);
    assert(!strcmp(s_engine.status.ip_addr, "0.0.0.0"));
    assert(atomic_load(&s_disconnect_epoch) == 1);
    run_worker_at(31499);
    assert(!s_engine.status.ap_active);
    run_worker_at(31500);
    assert(s_engine.status.ap_active);
    acquired.ip_info.ip.addr = 0x1700000a;
    got_ip_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &acquired);
    run_worker_at(32000);
    assert(s_engine.status.connected && s_engine.status.ap_active);
    assert(!strcmp(s_engine.status.ip_addr, "10.0.0.23"));
    run_worker_at(42000);
    assert(!s_engine.status.ap_active);
}
static void connected_with_setup_hotspot(void)
{
    reset_fixture(true);
    run_worker_at(0);
    run_worker_at(30000);
    assert(s_engine.status.ap_active);
    associated();
    ip_event_got_ip_t acquired = {.ip_info.ip.addr = 0x1600000a};
    got_ip_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &acquired);
    run_worker_at(31000);
    assert(s_status.connected && s_status.rssi == -40);
    assert(!strcmp(s_status.ip_addr, "10.0.0.22"));
    s_ap_info_calls = 0;
    s_network_updates = 0;
}
/* Missing periodic sampling leaves both manager and runtime signal frozen. */
static void signal_changes_are_published(void)
{
    connected_with_setup_hotspot();
    s_radio_rssi = -67;
    run_worker_at(36000);
    assert(s_status.rssi == -67);
    assert(s_published_rssi == -67 && s_network_updates == 1);
    assert(s_status.connected && !strcmp(s_published_ip, "10.0.0.22"));
}
/* Sampling every worker turn would unnecessarily query the radio at 10 Hz. */
static void signal_queries_are_rate_limited(void)
{
    connected_with_setup_hotspot();
    for (uint64_t now = 31100; now < 36000; now += 100) run_worker_at(now);
    assert(s_ap_info_calls == 0);
    run_worker_at(36000);
    assert(s_ap_info_calls == 1);
    for (uint64_t now = 36100; now < 41000; now += 100) run_worker_at(now);
    assert(s_ap_info_calls == 1);
    run_worker_at(41000);
    assert(s_ap_info_calls == 2);
    disconnected(WIFI_REASON_NO_AP_FOUND);
    run_worker_at(41100);
    run_worker_at(42099);
    assert(s_ap_info_calls == 2);
}
/* A transient query error must neither disconnect nor clear the last sample. */
static void signal_query_failure_preserves_connection(void)
{
    connected_with_setup_hotspot();
    s_ap_info_result = ESP_FAIL;
    run_worker_at(36000);
    assert(s_ap_info_calls == 1);
    assert(s_status.connected && s_status.rssi == -40);
    assert(!strcmp(s_status.ip_addr, "10.0.0.22"));
    assert(s_status.result == WIFI_RESULT_CONNECTED && s_network_updates == 0);
    run_worker_at(40999);
    assert(s_ap_info_calls == 1 && s_status.ap_active);
    s_ap_info_result = ESP_OK;
    s_radio_rssi = -55;
    run_worker_at(41000);
    assert(s_status.rssi == -55 && !s_status.ap_active && s_ap_stops == 1);
}
/* Treating a sample as another GOT_IP would indefinitely extend AP grace. */
static void signal_refresh_keeps_hotspot_deadline(void)
{
    connected_with_setup_hotspot();
    s_radio_rssi = -67;
    run_worker_at(36000);
    assert(s_status.rssi == -67);
    run_worker_at(40999);
    assert(s_status.ap_active && s_ap_stops == 0);
    s_radio_rssi = -70;
    run_worker_at(41000);
    assert(s_status.rssi == -70 && !s_status.ap_active && s_ap_stops == 1);
}
/* Radio association can change before the worker drains its DHCP event. */
static void signal_from_other_network_is_ignored(void)
{
    connected_with_setup_hotspot();
    s_reported_ssid = k_next.ssid;
    s_radio_rssi = -67;
    run_worker_at(36000);
    assert(s_ap_info_calls == 1);
    assert(s_status.connected && s_status.rssi == -40 && s_network_updates == 0);
    assert(!strcmp(s_status.ssid, k_old.ssid) && !strcmp(s_status.ip_addr, "10.0.0.22"));
    run_worker_at(41000);
    assert(!s_status.ap_active && s_ap_stops == 1);
}
/* A disconnect delivered during the radio query invalidates its sample. */
static void signal_query_racing_disconnect_is_ignored(void)
{
    connected_with_setup_hotspot();
    s_radio_rssi = -67;
    s_disconnect_during_query = true;
    run_worker_at(36000);
    assert(s_ap_info_calls == 1);
    assert(s_status.rssi == -40 && s_network_updates == 0);
    run_worker_at(36100);
    assert(!s_status.connected && s_status.rssi == 0);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    reset_fixture(true);
    for(service_operation_t owner=SERVICE_OPERATION_PRINTER_CONFIG;owner<=SERVICE_OPERATION_PRINTER_CONFIG;++owner){
        assert(service_operation_begin(owner));
        wifi_manager_status_t status;wifi_manager_get_status(&status);
        assert(!status.can_save && wifi_manager_request_credentials(&k_next)==ESP_ERR_NOT_ALLOWED);
        service_operation_end(owner);
    }
    if (strcmp(argv[1], "router") == 0) router_disconnect_before_dhcp_retries();
    else if (strcmp(argv[1], "association") == 0) association_clears_an_unreported_local_disconnect();
    else if (strcmp(argv[1], "replacement") == 0) credential_change_consumes_only_its_own_disconnect();
    else if (strcmp(argv[1], "failed_disconnect") == 0) failed_disconnect_does_not_mask_router_failure();
    else if (strcmp(argv[1], "ip_lost") == 0) lease_loss_opens_fallback_and_reacquires();
    else if (strcmp(argv[1], "rssi_refresh") == 0) signal_changes_are_published();
    else if (strcmp(argv[1], "rssi_rate") == 0) signal_queries_are_rate_limited();
    else if (strcmp(argv[1], "rssi_failure") == 0) signal_query_failure_preserves_connection();
    else if (strcmp(argv[1], "rssi_ap_deadline") == 0) signal_refresh_keeps_hotspot_deadline();
    else if (strcmp(argv[1], "rssi_other_network") == 0) signal_from_other_network_is_ignored();
    else if (strcmp(argv[1], "rssi_disconnect_race") == 0) signal_query_racing_disconnect_is_ignored();
    else assert(!"unknown Wi-Fi recovery scenario");
    puts("Native Wi-Fi disconnect recovery passed");
    return 0;
}
