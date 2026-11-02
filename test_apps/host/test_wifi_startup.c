#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep manager startup real; replace the radio, network-stack and RTOS
 * boundaries, including their persistent resource ownership. */
#include "../../components/wifi_manager/wifi_manager.c"

typedef enum {
    FAIL_NONE, FAIL_NVS, FAIL_NETIF, FAIL_EVENT_LOOP, FAIL_RADIO_INIT,
    FAIL_STORAGE, FAIL_WIFI_HANDLER, FAIL_IP_HANDLER, FAIL_AP_MODE,
    FAIL_AP_CONFIG, FAIL_STA_MODE, FAIL_RADIO_START, FAIL_WORKER
} failure_t;
static failure_t s_failure;
static bool s_netif_initialized, s_event_loop_created, s_radio_initialized;
static bool s_radio_started, s_ram_storage;
static wifi_mode_t s_radio_mode;
static wifi_config_t s_ap_config;
struct esp_netif { int interface; };
static struct esp_netif s_test_sta = {WIFI_IF_STA}, s_test_ap = {WIFI_IF_AP};
static bool s_sta_created, s_ap_created;
static unsigned s_duplicate_resources, s_worker_creations;
static esp_event_handler_t s_registered_wifi, s_registered_ip;
static unsigned s_wifi_registrations, s_ip_registrations;
static bool failing(failure_t point) { return s_failure == point; }

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, TickType_t wait)
{ (void)wait; assert(lock); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t lock) { assert(lock); return pdTRUE; }
BaseType_t xTaskNotifyGive(TaskHandle_t task) { assert(task); return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{ (void)clear; (void)wait; abort(); }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *out)
{
    assert(fn && name && stack && !arg && priority && out);
    assert(s_radio_started);
    if (failing(FAIL_WORKER)) return pdFALSE;
    ++s_worker_creations;
    *out = (TaskHandle_t)1;
    return pdPASS;
}
int64_t esp_timer_get_time(void) { return 0; }
void app_runtime_set_network_status(int8_t rssi, const char *ip)
{ (void)rssi; (void)ip; }
esp_err_t app_runtime_report_startup_fault(heat_fault_t fault)
{ (void)fault; return ESP_OK; }
esp_err_t device_config_init(void) { return ESP_OK; }
esp_err_t device_config_load_wifi(device_wifi_credentials_t *out)
{ return wifi_credentials_init(out, "home network", "home-password"); }
esp_err_t device_config_seed_wifi(const device_wifi_credentials_t *seed)
{ assert(seed); return ESP_OK; }
esp_err_t device_config_save_wifi(const device_wifi_credentials_t *credentials)
{ assert(credentials); return ESP_OK; }
esp_err_t nvs_flash_init(void) { return failing(FAIL_NVS) ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_flash_erase(void) { abort(); }
esp_err_t esp_netif_init(void)
{
    if (failing(FAIL_NETIF)) return ESP_FAIL;
    s_netif_initialized = true;
    return ESP_OK;
}
esp_err_t esp_event_loop_create_default(void)
{
    if (failing(FAIL_EVENT_LOOP)) return ESP_FAIL;
    if (s_event_loop_created) return ESP_ERR_INVALID_STATE;
    s_event_loop_created = true;
    return ESP_OK;
}
esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    assert(s_netif_initialized && s_event_loop_created);
    /* The SDK builder asserts instead of returning NULL on duplicate keys.
     * Return NULL here so the manager's violated contract has a clear failure. */
    if (s_sta_created) { ++s_duplicate_resources; return NULL; }
    s_sta_created = true;
    return &s_test_sta;
}
esp_netif_t *esp_netif_create_default_wifi_ap(void)
{
    assert(s_netif_initialized && s_event_loop_created);
    if (s_ap_created) { ++s_duplicate_resources; return NULL; }
    s_ap_created = true;
    return &s_test_ap;
}
esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    assert(config && s_sta_created && s_ap_created);
    if (s_radio_initialized) { ++s_duplicate_resources; return ESP_ERR_INVALID_STATE; }
    if (failing(FAIL_RADIO_INIT)) return ESP_FAIL;
    s_radio_initialized = true;
    return ESP_OK;
}
esp_err_t esp_wifi_set_storage(int storage)
{
    assert(s_radio_initialized && !s_radio_started && storage == WIFI_STORAGE_RAM);
    if (failing(FAIL_STORAGE)) return ESP_FAIL;
    s_ram_storage = true;
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    esp_event_handler_t handler, void *arg, void *instance)
{
    assert(s_radio_initialized && handler && !arg && !instance);
    if (strcmp(base, WIFI_EVENT) == 0) {
        assert(id == ESP_EVENT_ANY_ID);
        if (failing(FAIL_WIFI_HANDLER)) return ESP_FAIL;
        s_registered_wifi = handler;
        ++s_wifi_registrations;
    } else {
        assert(strcmp(base, IP_EVENT) == 0 && id == IP_EVENT_STA_GOT_IP);
        if (failing(FAIL_IP_HANDLER)) return ESP_FAIL;
        s_registered_ip = handler;
        ++s_ip_registrations;
    }
    return ESP_OK;
}
esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    assert(s_radio_initialized && !s_radio_started);
    if ((mode == WIFI_MODE_APSTA && failing(FAIL_AP_MODE)) ||
        (mode == WIFI_MODE_STA && failing(FAIL_STA_MODE))) return ESP_FAIL;
    s_radio_mode = mode;
    return ESP_OK;
}
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config)
{
    assert(interface == WIFI_IF_AP && config && s_radio_mode == WIFI_MODE_APSTA);
    if (failing(FAIL_AP_CONFIG)) return ESP_FAIL;
    s_ap_config = *config;
    return ESP_OK;
}
esp_err_t esp_wifi_start(void)
{
    assert(s_radio_initialized && s_ram_storage && !s_radio_started);
    assert(s_radio_mode == WIFI_MODE_STA);
    /* No factory/default AP may be exposed, even on a retry. */
    assert(s_ap_config.ap.ssid_len == 14);
    assert(memcmp(s_ap_config.ap.ssid, "P1S-Heat-Setup", 14) == 0);
    assert(s_ap_config.ap.channel == 1 && s_ap_config.ap.authmode == WIFI_AUTH_OPEN);
    assert(s_ap_config.ap.max_connection == 4);
    assert(s_registered_wifi && s_registered_ip);
    if (failing(FAIL_RADIO_START)) return ESP_FAIL;
    s_radio_started = true;
    return ESP_OK;
}
esp_err_t esp_wifi_get_config(int interface, wifi_config_t *config)
{ (void)interface; (void)config; return ESP_FAIL; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{ (void)ap; return ESP_FAIL; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip)
{ (void)netif; (void)ip; return ESP_FAIL; }
esp_err_t esp_wifi_connect(void) { assert(s_radio_started); return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
esp_err_t mdns_init(void) { return ESP_OK; }
esp_err_t mdns_hostname_set(const char *name) { assert(name); return ESP_OK; }
esp_err_t mdns_instance_name_set(const char *name) { assert(name); return ESP_OK; }
esp_err_t mdns_service_add(const char *instance, const char *service, const char *proto,
    unsigned port, const void *txt, unsigned count)
{
    assert(!instance && service && proto && port == 80 && !txt && count == 0);
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    static const char *const cases[] = {"none", "nvs", "netif", "event_loop", "radio_init",
        "storage", "wifi_handler", "ip_handler", "ap_mode", "ap_config", "sta_mode",
        "radio_start", "worker"};
    unsigned selected;
    for (selected = 0; selected < sizeof(cases) / sizeof(cases[0]); ++selected)
        if (strcmp(argv[1], cases[selected]) == 0) break;
    assert(selected < sizeof(cases) / sizeof(cases[0]));
    if (selected == FAIL_NONE) s_event_loop_created = true;
    else {
        s_failure = (failure_t)selected;
        const esp_err_t expected = selected == FAIL_WORKER ? ESP_ERR_NO_MEM : ESP_FAIL;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const esp_err_t result = wifi_manager_start();
            if (result != expected) fprintf(stderr,
                "%s retry %u: expected injected error %d, got %d (duplicate resources: %u)\n",
                argv[1], attempt, expected, result, s_duplicate_resources);
            assert(result == expected);
            assert(s_worker_creations == 0 && !s_mdns_ready);
        }
    }
    s_failure = FAIL_NONE;
    assert(wifi_manager_start() == ESP_OK);
    assert(wifi_manager_start() == ESP_OK);
    assert(s_duplicate_resources == 0 && s_worker_creations == 1);
    assert(s_wifi_registrations == 1 && s_ip_registrations == 1);
    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    assert(status.can_save && status.configured);
    assert(strcmp(status.ssid, "home network") == 0);
    const wifi_event_sta_disconnected_t disconnected = {.reason = WIFI_REASON_NO_AP_FOUND};
    s_registered_wifi(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, (void *)&disconnected);
    assert(s_event.pending && s_event.result == WIFI_RESULT_NO_NETWORK);
    puts("Wi-Fi startup retries passed");
    return 0;
}
