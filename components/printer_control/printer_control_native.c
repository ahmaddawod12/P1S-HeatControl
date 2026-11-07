#include "app_runtime.h"
#include "device_config.h"
#include "esp_timer.h"
#include "printer_control.h"
#include "printer_link.h"
static void heat(app_status_t *out, void *c) {
  (void)c;
  app_runtime_get_status(out);
}
static esp_err_t begin(uint64_t until, void *c) {
  (void)c;
  return app_runtime_monitor_begin(until);
}
static esp_err_t permit(uint64_t until, void *c) {
  (void)c;
  return app_runtime_monitor_permit(until);
}
static void end_monitor(bool disable, void *c) {
  (void)c;
  app_runtime_monitor_end(disable);
}
static bool monitor_active(void *c) {
  (void)c;
  return app_runtime_monitor_active();
}
static esp_err_t configure(const printer_connection_config_t *v, void *c) {
  (void)c;
  return printer_link_configure(v);
}
static void tick(uint64_t at, void *c) {
  (void)c;
  printer_link_tick(at);
}
static void link_status(printer_link_status_t *out, void *c) {
  (void)c;
  printer_link_get_status(out);
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
static bool gate_begin(service_operation_t op, void *c) {
  (void)c;
  return service_operation_begin(op);
}
static void gate_end(service_operation_t op, void *c) {
  (void)c;
  service_operation_end(op);
}
static uint64_t now(void *c) {
  (void)c;
  return (uint64_t)(esp_timer_get_time() / 1000);
}
esp_err_t printer_control_start(void) {
  static bool ready;
  if (ready)
    return ESP_OK;
  esp_err_t result = printer_link_start();
  if (result != ESP_OK)
    return result;
  const printer_control_ops_t ops = {.heat_snapshot = heat,
                                     .monitor_begin = begin,
                                     .monitor_permit = permit,
                                     .monitor_end = end_monitor,
                                     .monitor_active = monitor_active,
                                     .link_configure = configure,
                                     .link_tick = tick,
                                     .link_status = link_status,
                                     .load_config = load,
                                     .save_config = save,
                                     .clear_config = clear,
                                     .gate_begin = gate_begin,
                                     .gate_end = gate_end,
                                     .now_ms = now};
  result = printer_control_start_with_ops(&ops);
  ready = result == ESP_OK;
  return result;
}
