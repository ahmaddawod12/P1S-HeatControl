#include "app_runtime.h"
#include "device_config.h"
#include "printer_control.h"
#include "printer_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static printer_control_ops_t ops;
static app_status_t heat;
static runtime_auto_status_t automatic;
static bool (*guard)(void *);
static void *guard_ctx;
static uint64_t now = 1000;
static const printer_safety_history_t dispatch_history = {
    .pause = 2, .error = 3, .identity = 4};
int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
void esp_fill_random(void *out, size_t n) { memset(out, 0x23, n); }
void app_runtime_get_status(app_status_t *out) { *out = heat; }
void app_runtime_get_auto_status(runtime_auto_status_t *out) {
  *out = automatic;
}
esp_err_t app_runtime_auto_acquire(uint64_t id, float target, uint64_t expiry) {
  automatic = (runtime_auto_status_t){
      .active = true, .cycle_id = id, .expires_ms = expiry};
  heat.enabled = true;
  heat.target_c = target;
  return ESP_OK;
}
esp_err_t app_runtime_auto_renew(uint64_t id, uint64_t expiry) {
  (void)id;
  automatic.expires_ms = expiry;
  return ESP_OK;
}
void app_runtime_auto_revoke(uint64_t id) {
  (void)id;
  automatic.active = false;
}
esp_err_t printer_control_start_with_ops(const printer_control_ops_t *value) {
  ops = *value;
  return ESP_OK;
}
esp_err_t printer_link_start(void) { return ESP_OK; }
void printer_link_set_dispatch_guard(bool (*value)(void *), void *ctx) {
  guard = value;
  guard_ctx = ctx;
}
esp_err_t printer_link_configure(const printer_connection_config_t *value) {
  (void)value;
  return ESP_OK;
}
void printer_link_tick(uint64_t at) { (void)at; }
void printer_link_snapshot(printer_observation_t *out) {
  memset(out, 0, sizeof(*out));
}
bool printer_link_connection_live(uint32_t epoch) {
  (void)epoch;
  return true;
}
void printer_link_cancel_resume(void) {}
printer_dispatch_result_t printer_link_resume_once(uint64_t id,
                                                   const printer_job_key_t *job,
                                                   const printer_safety_history_t *history,
                                                   uint32_t epoch,
                                                   uint64_t deadline) {
  (void)id;
  (void)job;
  (void)epoch;
  (void)deadline;
  assert(history && history->pause == 2 && history->error == 3 &&
         history->identity == 4);
  return guard(guard_ctx) ? PRINTER_DISPATCH_SENT : PRINTER_DISPATCH_REJECTED;
}
esp_err_t device_config_load_printer(printer_connection_config_t *out) {
  (void)out;
  return ESP_ERR_NOT_FOUND;
}
esp_err_t device_config_save_printer(const printer_connection_config_t *value) {
  (void)value;
  return ESP_OK;
}
esp_err_t device_config_clear_printer(void) { return ESP_OK; }
bool service_operation_begin(service_operation_t value) {
  (void)value;
  return true;
}
void service_operation_end(service_operation_t value) { (void)value; }
static printer_dispatch_result_t dispatch(void) {
  printer_job_key_t job = {.identity_valid = true};
  return ops.link_resume(7, &job, &dispatch_history, 1, now + 1000, ops.ctx);
}
int main(void) {
  heat = app_status_initial();
  heat.temperature = (temperature_sample_t){.valid = true,
                                            .consecutive_valid = 3,
                                            .raw_c = 60,
                                            .filtered_c = 60,
                                            .timestamp_ms = now};
  assert(printer_control_start() == ESP_OK && guard);
  assert(ops.heat_acquire(7, 60, now + 5000, ops.ctx) == ESP_OK);
  assert(dispatch() == PRINTER_DISPATCH_SENT);
  /* Actual native guard, after the link's waits, must retain the captured
   * target. */
  heat.temperature.filtered_c = 58;
  assert(dispatch() == PRINTER_DISPATCH_REJECTED);
  heat.temperature.filtered_c = 61;
  assert(dispatch() == PRINTER_DISPATCH_SENT);
  heat.temperature.filtered_c = 61.01f;
  assert(dispatch() == PRINTER_DISPATCH_REJECTED);
  heat.target_c = heat.temperature.filtered_c = 59;
  assert(dispatch() == PRINTER_DISPATCH_REJECTED);
  heat.target_c = heat.temperature.filtered_c = 60;
  automatic.expires_ms = now;
  assert(dispatch() == PRINTER_DISPATCH_REJECTED);
  puts("native Resume readiness guard: ok");
  return 0;
}
