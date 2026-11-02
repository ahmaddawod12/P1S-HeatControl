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
/* Real coordinator task/mutex/service gate; all printer, storage and heat
 * boundaries are synthetic. This case never touches relays or a printer. */
static atomic_uint acquisitions, resumes, random_sequence;
static uint64_t now(void *ctx) {
  (void)ctx;
  return (uint64_t)(esp_timer_get_time() / 1000);
}
static esp_err_t acquire(uint64_t id, float target, uint64_t expiry,
                         void *ctx) {
  (void)id;
  (void)target;
  (void)expiry;
  (void)ctx;
  atomic_fetch_add(&acquisitions, 1);
  return ESP_ERR_NOT_ALLOWED; /* e.g. a pending local target command */
}
static esp_err_t renew(uint64_t id, uint64_t expiry, void *ctx) {
  (void)id;
  (void)expiry;
  (void)ctx;
  return ESP_ERR_INVALID_STATE;
}
static void revoke(uint64_t id, void *ctx) {
  (void)id;
  (void)ctx;
}
static void heat(app_status_t *out, void *ctx) {
  *out = app_status_initial();
  out->target_c = 60;
  out->temperature = (temperature_sample_t){.valid = true,
                                            .consecutive_valid = 3,
                                            .raw_c = 60,
                                            .filtered_c = 60,
                                            .timestamp_ms = now(ctx)};
}
static void automatic(runtime_auto_status_t *out, void *ctx) {
  (void)ctx;
  *out = (runtime_auto_status_t){.teardown_applied = true};
}
static esp_err_t configure(const printer_connection_config_t *value,
                           void *ctx) {
  (void)value;
  (void)ctx;
  return ESP_OK;
}
static void tick(uint64_t at, void *ctx) {
  (void)at;
  (void)ctx;
}
static void snapshot(printer_observation_t *out, void *ctx) {
  *out = (printer_observation_t){.connected = true,
                                 .fresh = true,
                                 .baseline_valid = true,
                                 .startup_eligible = true,
                                 .state = PRINTER_RUN_PAUSED,
                                 .connection_epoch = 1,
                                 .received_ms = now(ctx),
                                 .job = {.identity_valid = true,
                                         .connection_epoch = 1,
                                         .print_epoch = 1,
                                         .start_time = 100,
                                         .label = "synthetic"}};
}
static printer_dispatch_result_t resume(uint64_t id,
                                        const printer_job_key_t *job,
                                        const printer_safety_history_t *history,
                                        uint32_t epoch, uint64_t deadline,
                                        void *ctx) {
  (void)id;
  (void)job;
  (void)history;
  (void)epoch;
  (void)deadline;
  (void)ctx;
  atomic_fetch_add(&resumes, 1);
  return PRINTER_DISPATCH_REJECTED;
}
static esp_err_t load(printer_connection_config_t *out, void *ctx) {
  (void)ctx;
  *out = (printer_connection_config_t){.enabled = true,
                                       .ipv4 = "192.168.1.20",
                                       .serial = "SYNTHETIC",
                                       .access_code = "synthetic",
                                       .ntp_host = "pool.ntp.org"};
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
static bool begin(service_operation_t owner, void *ctx) {
  (void)ctx;
  return service_operation_begin(owner);
}
static void end(service_operation_t owner, void *ctx) {
  (void)ctx;
  service_operation_end(owner);
}
static void random_bytes(void *out, size_t n, void *ctx) {
  (void)ctx;
  unsigned sequence = atomic_fetch_add(&random_sequence, 1);
  unsigned char *p = out;
  for (size_t i = 0; i < n; ++i)
    p[i] = (unsigned char)(sequence + i * 17);
}
TEST_CASE("printer boot restores monitoring without heat or Resume",
          "[printer_control]") {
  static const printer_control_ops_t ops = {.heat_acquire = acquire,
                                            .heat_renew = renew,
                                            .heat_revoke = revoke,
                                            .heat_snapshot = heat,
                                            .auto_snapshot = automatic,
                                            .link_configure = configure,
                                            .link_tick = tick,
                                            .link_snapshot = snapshot,
                                            .link_resume = resume,
                                            .load_config = load,
                                            .save_config = save,
                                            .clear_config = clear,
                                            .gate_begin = begin,
                                            .gate_end = end,
                                            .now_ms = now,
                                            .random_bytes = random_bytes,
                                            .resume_qualified = true};
  TEST_ESP_OK(printer_control_start_with_ops(&ops));
  printer_control_status_t status = {0};
  uint64_t deadline = now(NULL) + 2000;
  do {
    vTaskDelay(pdMS_TO_TICKS(100));
    printer_control_get_status(&status);
  } while (!status.can_arm && now(NULL) < deadline);
  TEST_ASSERT_TRUE(status.configured && status.can_arm);
  TEST_ASSERT_EQUAL_UINT32(0, atomic_load(&acquisitions));
  TEST_ASSERT_EQUAL_UINT32(0, atomic_load(&resumes));
  printer_arm_request_t request = {.initial_pause_confirmed = true};
  strcpy(request.arm_token, status.cycle.arm_token);
  TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, printer_control_arm(&request));
  TEST_ASSERT_EQUAL(SERVICE_OPERATION_NONE, service_operation_current());
  TEST_ASSERT_EQUAL_UINT32(1, atomic_load(&acquisitions));
  TEST_ESP_OK(printer_control_stop());
  TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, printer_control_arm(&request));
  TEST_ASSERT_EQUAL_UINT32(1, atomic_load(&acquisitions));
  TEST_ASSERT_EQUAL_UINT32(0, atomic_load(&resumes));
}
