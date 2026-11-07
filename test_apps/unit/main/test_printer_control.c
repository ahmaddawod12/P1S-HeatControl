#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "printer_control.h"
#include "unity.h"
#include <stdatomic.h>
#include <string.h>
TEST_CASE("MQTT queue has room for a dispatch and an SDK event",
          "[printer_control]") {
  const esp_mqtt_client_config_t config = {.broker.address.uri =
                                               "mqtt://127.0.0.1:9"};
  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
  TEST_ASSERT_NOT_NULL(client);
  esp_mqtt_event_t first = {.msg_id = 1}, second = {.msg_id = 2};
  const esp_err_t first_result = esp_mqtt_dispatch_custom_event(client, &first);
  const esp_err_t second_result =
      esp_mqtt_dispatch_custom_event(client, &second);
  /* No client task is started: this probes queue capacity without a connection.
   */
  TEST_ESP_OK(esp_mqtt_client_destroy(client));
  TEST_ESP_OK(first_result);
  TEST_ESP_OK(second_result);
}
/* Real coordinator task/mutex; all printer, storage and heat boundaries are
 * synthetic. This case never touches relays or a printer. */
static atomic_uint begins, ends;
static atomic_bool active;
static uint64_t now(void *ctx) {
  (void)ctx;
  return (uint64_t)(esp_timer_get_time() / 1000);
}
static void heat(app_status_t *out, void *ctx) {
  (void)ctx;
  *out = app_status_initial();
  out->state = HEAT_STATE_HEATING;
  out->enabled = true;
}
static esp_err_t monitor_begin(uint64_t until, void *ctx) {
  (void)until;
  (void)ctx;
  atomic_fetch_add(&begins, 1);
  atomic_store(&active, true);
  return ESP_OK;
}
static esp_err_t monitor_permit(uint64_t until, void *ctx) {
  (void)until;
  (void)ctx;
  return atomic_load(&active) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
static void monitor_end(bool disable, void *ctx) {
  (void)disable;
  (void)ctx;
  atomic_fetch_add(&ends, 1);
  atomic_store(&active, false);
}
static bool monitor_active(void *ctx) {
  (void)ctx;
  return atomic_load(&active);
}
static esp_err_t configure(const printer_connection_config_t *value, void *ctx) {
  (void)value;
  (void)ctx;
  return ESP_OK;
}
static void tick(uint64_t at, void *ctx) {
  (void)at;
  (void)ctx;
}
static void link_status(printer_link_status_t *out, void *ctx) {
  (void)ctx;
  *out = (printer_link_status_t){.connected = true, .state_known = true,
                                 .state = PRINTER_RUN_RUNNING,
                                 .received_ms = now(NULL)};
}
static esp_err_t load(printer_connection_config_t *out, void *ctx) {
  (void)ctx;
  *out = (printer_connection_config_t){.enabled = true,
                                       .ipv4 = "192.168.1.20",
                                       .serial = "SYNTHETIC",
                                       .access_code = "synthetic"};
  return ESP_OK;
}
static esp_err_t save(const printer_connection_config_t *value, void *ctx) {
  (void)value;
  (void)ctx;
  return ESP_ERR_INVALID_STATE;
}
static esp_err_t clear(void *ctx) {
  (void)ctx;
  return ESP_ERR_INVALID_STATE;
}
static bool gate_begin(service_operation_t owner, void *ctx) {
  (void)ctx;
  return service_operation_begin(owner);
}
static void gate_end(service_operation_t owner, void *ctx) {
  (void)ctx;
  service_operation_end(owner);
}
TEST_CASE("printer boot restores monitoring readiness without monitoring",
          "[printer_control]") {
  static const printer_control_ops_t ops = {
      .heat_snapshot = heat, .monitor_begin = monitor_begin,
      .monitor_permit = monitor_permit, .monitor_end = monitor_end,
      .monitor_active = monitor_active, .link_configure = configure,
      .link_tick = tick, .link_status = link_status, .load_config = load,
      .save_config = save, .clear_config = clear, .gate_begin = gate_begin,
      .gate_end = gate_end, .now_ms = now};
  TEST_ESP_OK(printer_control_start_with_ops(&ops));
  printer_control_status_t status = {0};
  uint64_t deadline = now(NULL) + 2000;
  do {
    vTaskDelay(pdMS_TO_TICKS(100));
    printer_control_get_status(&status);
  } while (!status.can_monitor && now(NULL) < deadline);
  TEST_ASSERT_TRUE(status.configured && status.can_monitor);
  TEST_ASSERT_EQUAL_UINT32(0, atomic_load(&begins));
  TEST_ESP_OK(printer_control_monitor_start());
  TEST_ASSERT_EQUAL_UINT32(1, atomic_load(&begins));
  TEST_ASSERT_EQUAL(SERVICE_OPERATION_NONE, service_operation_current());
  TEST_ESP_OK(printer_control_monitor_stop());
  TEST_ASSERT_EQUAL_UINT32(1, atomic_load(&ends));
  TEST_ESP_OK(printer_control_monitor_stop());
  TEST_ASSERT_EQUAL_UINT32(1, atomic_load(&ends));
}
