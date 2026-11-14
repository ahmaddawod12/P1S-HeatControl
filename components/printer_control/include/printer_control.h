#pragma once
#include "app_types.h"
#include "esp_err.h"
#include "printer_types.h"
#include "service_operation.h"
/* Monitor P1S: manual heating that turns itself off when the print ends.
 * Injection boundary for the runtime, printer link and configuration storage;
 * production wiring lives in printer_control_native.c. */
typedef struct {
  void (*heat_snapshot)(app_status_t *, void *);
  esp_err_t (*monitor_begin)(uint64_t permit_until_ms, void *);
  esp_err_t (*monitor_permit)(uint64_t permit_until_ms, void *);
  void (*monitor_end)(bool disable_heating, void *);
  bool (*monitor_active)(void *);
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
/* Stops monitoring and keeps manual heating; ESP_OK when already off. */
esp_err_t printer_control_monitor_stop(void);
void printer_control_get_status(printer_control_status_t *);
