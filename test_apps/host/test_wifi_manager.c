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
{ (void)rssi; (void)ip; }
esp_err_t device_config_save_wifi(const device_wifi_credentials_t *credentials)
{ assert(credentials); return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_get_config(int interface, wifi_config_t *config)
{ assert(interface == WIFI_IF_STA); *config = s_radio_config; return ESP_OK; }
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config)
{ assert(interface == WIFI_IF_STA); s_radio_config = *config; return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{
    if (!s_associated) return ESP_FAIL;
    memset(ap, 0, sizeof(*ap));
    memcpy(ap->ssid, s_radio_config.sta.ssid, sizeof(s_radio_config.sta.ssid));
    ap->rssi = -40;
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
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "router") == 0) router_disconnect_before_dhcp_retries();
    else if (strcmp(argv[1], "association") == 0) association_clears_an_unreported_local_disconnect();
    else if (strcmp(argv[1], "replacement") == 0) credential_change_consumes_only_its_own_disconnect();
    else if (strcmp(argv[1], "failed_disconnect") == 0) failed_disconnect_does_not_mask_router_failure();
    else assert(!"unknown Wi-Fi recovery scenario");
    puts("Native Wi-Fi disconnect recovery passed");
    return 0;
}
