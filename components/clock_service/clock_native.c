#include "clock_service_ops.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
static uint64_t mono(void) { return (uint64_t)(esp_timer_get_time() / 1000); }
static bool valid(void) { return time(NULL) >= 1704067200; }
static esp_err_t start(const char *host) {
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(host);
  return esp_netif_sntp_init(&config);
}
static void wait_clock(uint32_t ms) {
  (void)esp_netif_sntp_sync_wait(pdMS_TO_TICKS(ms));
}
static void stop(void) { esp_netif_sntp_deinit(); }
static void sleep_clock(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
esp_err_t clock_service_wait(const char *host, uint32_t timeout,
                             bool (*cancelled)(void *), void *ctx) {
  const clock_service_ops_t ops = {mono,       valid, start,
                                   wait_clock, stop,  sleep_clock};
  return clock_service_wait_with_ops(&ops, host, timeout, cancelled, ctx);
}
