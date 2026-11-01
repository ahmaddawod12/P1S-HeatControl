/* Startup is outside these callback/worker tests. Fail loudly if a case
 * accidentally tries to initialize a real SDK service. */
#include <assert.h>
#include <stddef.h>
#include "app_runtime.h"
#include "device_config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#define UNAVAILABLE() (assert(!"native startup is unavailable in this test"), ESP_FAIL)
esp_err_t nvs_flash_init(void) { return UNAVAILABLE(); }
esp_err_t nvs_flash_erase(void) { return UNAVAILABLE(); }
esp_err_t esp_netif_init(void) { return UNAVAILABLE(); }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { (void)UNAVAILABLE(); return NULL; }
esp_netif_t *esp_netif_create_default_wifi_ap(void) { (void)UNAVAILABLE(); return NULL; }
esp_err_t esp_event_loop_create_default(void) { return UNAVAILABLE(); }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    esp_event_handler_t handler, void *arg, void *instance)
{ (void)base; (void)id; (void)handler; (void)arg; (void)instance; return UNAVAILABLE(); }
esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{ (void)config; return UNAVAILABLE(); }
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; return UNAVAILABLE(); }
esp_err_t esp_wifi_start(void) { return UNAVAILABLE(); }
esp_err_t mdns_init(void) { return UNAVAILABLE(); }
esp_err_t mdns_hostname_set(const char *name) { (void)name; return UNAVAILABLE(); }
esp_err_t mdns_instance_name_set(const char *name) { (void)name; return UNAVAILABLE(); }
esp_err_t mdns_service_add(const char *instance, const char *service, const char *proto,
    unsigned port, const void *txt, unsigned count)
{ (void)instance; (void)service; (void)proto; (void)port; (void)txt; (void)count; return UNAVAILABLE(); }
esp_err_t device_config_init(void) { return UNAVAILABLE(); }
esp_err_t device_config_load_wifi(device_wifi_credentials_t *out)
{ (void)out; return UNAVAILABLE(); }
esp_err_t device_config_seed_wifi(const device_wifi_credentials_t *seed)
{ (void)seed; return UNAVAILABLE(); }
esp_err_t app_runtime_report_startup_fault(heat_fault_t fault)
{ (void)fault; return UNAVAILABLE(); }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *out)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)out; return UNAVAILABLE(); }
