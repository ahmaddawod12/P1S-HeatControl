#pragma once
#include "app_types.h"
#include "esp_err.h"
#include "printer_types.h"
#include "runtime_auto_types.h"
#include "service_operation.h"
typedef struct {
  esp_err_t (*heat_acquire)(uint64_t, float, uint64_t, void *);
  esp_err_t (*heat_renew)(uint64_t, uint64_t, void *);
  void (*heat_revoke)(uint64_t, void *);
  void (*heat_snapshot)(app_status_t *, void *);
  void (*auto_snapshot)(runtime_auto_status_t *, void *);
  esp_err_t (*link_configure)(const printer_connection_config_t *, void *);
  void (*link_tick)(uint64_t, void *);
  void (*link_snapshot)(printer_observation_t *, void *);
  printer_dispatch_result_t (*link_resume)(uint64_t, const printer_job_key_t *,
                                           const printer_safety_history_t *,
                                           uint32_t, uint64_t, void *);
  void (*link_cancel)(void *);
  bool (*link_live)(uint32_t,void *);
  esp_err_t (*load_config)(printer_connection_config_t *, void *);
  esp_err_t (*save_config)(const printer_connection_config_t *, void *);
  esp_err_t (*clear_config)(void *);
  bool (*gate_begin)(service_operation_t, void *);
  void (*gate_end)(service_operation_t, void *);
  uint64_t (*now_ms)(void *);
  void (*random_bytes)(void *, size_t, void *);
  bool resume_qualified;
  void *ctx;
} printer_control_ops_t;
esp_err_t printer_control_start(void);
esp_err_t printer_control_start_with_ops(const printer_control_ops_t *);
esp_err_t printer_control_request_config(const printer_config_request_t *);
esp_err_t printer_control_forget_config(void);
esp_err_t printer_control_arm(const printer_arm_request_t *);
esp_err_t printer_control_stop(void);
void printer_control_get_status(printer_control_status_t *);
