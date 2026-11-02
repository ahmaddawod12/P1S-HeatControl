#include "app_runtime.h"
#include "device_config.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "printer_control.h"
#include "printer_cycle.h"
#include "printer_link.h"
#include "sdkconfig.h"
static uint64_t guard_cycle_id;
static uint64_t acquired_cycle_id;
static float acquired_target_c;
static bool dispatch_guard(void *ctx) {
  (void)ctx;
  runtime_auto_status_t automatic;
  app_status_t heat;
  app_runtime_get_auto_status(&automatic);
  app_runtime_get_status(&heat);
  uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
  return automatic.active && automatic.cycle_id == guard_cycle_id &&
         acquired_cycle_id == guard_cycle_id && automatic.expires_ms > now &&
         printer_cycle_dispatch_ready(&heat, acquired_target_c, now);
}
static esp_err_t acquire(uint64_t id, float target, uint64_t expiry, void *c) {
  (void)c;
  esp_err_t result = app_runtime_auto_acquire(id, target, expiry);
  if (result == ESP_OK) {
    acquired_cycle_id = id;
    acquired_target_c = target;
  }
  return result;
}
static esp_err_t renew(uint64_t id, uint64_t expiry, void *c) {
  (void)c;
  return app_runtime_auto_renew(id, expiry);
}
static void revoke(uint64_t id, void *c) {
  (void)c;
  app_runtime_auto_revoke(id);
}
static void heat(app_status_t *out, void *c) {
  (void)c;
  app_runtime_get_status(out);
}
static void automatic(runtime_auto_status_t *out, void *c) {
  (void)c;
  app_runtime_get_auto_status(out);
}
static esp_err_t configure(const printer_connection_config_t *v, void *c) {
  (void)c;
  return printer_link_configure(v);
}
static void tick(uint64_t at, void *c) {
  (void)c;
  printer_link_tick(at);
}
static void snapshot(printer_observation_t *out, void *c) {
  (void)c;
  printer_link_snapshot(out);
}
static printer_dispatch_result_t resume(uint64_t id,
                                        const printer_job_key_t *job,
                                        const printer_safety_history_t *history,
                                        uint32_t epoch, uint64_t deadline,
                                        void *c) {
  (void)c;
  guard_cycle_id = id;
  return printer_link_resume_once(id, job, history, epoch, deadline);
}
static void cancel(void *c) {
  (void)c;
  printer_link_cancel_resume();
}
static bool live(uint32_t epoch, void *c) {
  (void)c;
  return printer_link_connection_live(epoch);
}
static esp_err_t load(printer_connection_config_t *out, void *c) {
  (void)c;
  return device_config_load_printer(out);
}
static esp_err_t save(const printer_connection_config_t *v, void *c) {
  (void)c;
  return device_config_save_printer(v);
}
static esp_err_t clear(void *c) {
  (void)c;
  return device_config_clear_printer();
}
static bool begin(service_operation_t op, void *c) {
  (void)c;
  return service_operation_begin(op);
}
static void end(service_operation_t op, void *c) {
  (void)c;
  service_operation_end(op);
}
static uint64_t now(void *c) {
  (void)c;
  return (uint64_t)(esp_timer_get_time() / 1000);
}
static void random_bytes(void *out, size_t n, void *c) {
  (void)c;
  esp_fill_random(out, n);
}
esp_err_t printer_control_start(void) {
  static bool ready;
  if (ready)
    return ESP_OK;
  esp_err_t result = printer_link_start();
  if (result != ESP_OK)
    return result;
  printer_link_set_dispatch_guard(dispatch_guard, NULL);
  const printer_control_ops_t ops = {.heat_acquire = acquire,
                                     .heat_renew = renew,
                                     .heat_revoke = revoke,
                                     .heat_snapshot = heat,
                                     .auto_snapshot = automatic,
                                     .link_configure = configure,
                                     .link_tick = tick,
                                     .link_snapshot = snapshot,
                                     .link_resume = resume,
                                     .link_cancel = cancel,
                                     .link_live = live,
                                     .load_config = load,
                                     .save_config = save,
                                     .clear_config = clear,
                                     .gate_begin = begin,
                                     .gate_end = end,
                                     .now_ms = now,
                                     .random_bytes = random_bytes,
#ifdef CONFIG_P1S_PRINTER_AUTO_RESUME
                                     .resume_qualified = true
#else
                                     .resume_qualified = false
#endif
  };
  result = printer_control_start_with_ops(&ops);
  ready = result == ESP_OK;
  return result;
}
