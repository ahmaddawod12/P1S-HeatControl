#pragma once
#include "app_types.h"
#include "esp_err.h"
#include "printer_types.h"
#include "service_operation.h"
struct app_runtime_enable_guard;

/* Monitor P1S: manual heating that turns itself off when the print ends.
 * Injection boundary for the runtime, printer link and configuration storage;
 * production wiring lives in printer_control_native.c. */
typedef struct {
  void (*heat_snapshot)(app_status_t *, void *);
  esp_err_t (*monitor_begin)(printer_monitor_phase_t, uint64_t permit_until_ms,
                             uint64_t deadline_ms, void *);
  esp_err_t (*monitor_permit)(printer_monitor_phase_t, uint64_t permit_until_ms,
                              void *);
  esp_err_t (*monitor_suspend_recovery)(void *);
  esp_err_t (*monitor_end)(bool disable_heating, uint64_t deadline_ms,
                          const struct app_runtime_enable_guard *guard, uint32_t revision, void *);
  printer_monitor_phase_t (*monitor_phase)(void *);
  esp_err_t (*link_configure)(const printer_connection_config_t *, void *);
  void (*link_tick)(uint64_t, void *);
  void (*link_status)(printer_link_status_t *, void *);
  esp_err_t (*load_config)(printer_connection_config_t *, void *);
  esp_err_t (*save_config)(const printer_connection_config_t *, void *);
  esp_err_t (*clear_config)(void *);
  bool (*gate_begin)(service_operation_t, void *);
  void (*gate_end)(service_operation_t, void *);
  uint64_t (*now_ms)(void *);
  void *ctx;
} printer_control_ops_t;
esp_err_t printer_control_start(void);
esp_err_t printer_control_start_with_ops(const printer_control_ops_t *);
esp_err_t printer_control_request_config(const printer_config_request_t *);
/* Asynchronous admission; applying/config_result report storage completion. */
esp_err_t printer_control_forget_config(void);
/* ESP_ERR_NOT_ALLOWED when not ready (see status reason). */
esp_err_t printer_control_monitor_start(void);
esp_err_t printer_control_monitor_start_until(uint64_t deadline_ms);
/* Stops monitoring and keeps manual heating; ESP_OK when already off. */
esp_err_t printer_control_monitor_stop(void);
esp_err_t printer_control_monitor_stop_until(uint64_t deadline_ms,
    const struct app_runtime_enable_guard *guard, uint32_t revision);
void printer_control_get_status(printer_control_status_t *);
