#include "clock_service_ops.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
static uint64_t mono(void) { return (uint64_t)(esp_timer_get_time() / 1000); }
bool clock_service_has_valid_wall_time(void) { return time(NULL) >= 1704067200; }
static esp_err_t start(const char *host) {
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(host);
  return esp_netif_sntp_init(&config);
}
static bool wait_clock(uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  return esp_netif_sntp_sync_wait(ticks ? ticks : 1U) == ESP_OK;
}
static void stop(void) { esp_netif_sntp_deinit(); }
static void sleep_clock(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static const clock_service_ops_t ops = {mono, clock_service_has_valid_wall_time, start, wait_clock, stop, sleep_clock};

esp_err_t clock_service_wait(const char *host, uint32_t timeout,
                             bool (*cancelled)(void *), void *ctx) {
  return clock_service_wait_with_ops(&ops, host, timeout, cancelled, ctx);
}

esp_err_t clock_service_sync(const char *host, uint32_t timeout) {
  return clock_service_sync_with_ops(&ops, host, timeout);
}

bool clock_service_is_recent(uint32_t max_age_seconds) {
  return clock_service_is_recent_with_ops(&ops, max_age_seconds);
}
