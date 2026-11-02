#pragma once
#include "esp_err.h"
#include "printer_types.h"
typedef struct {
  esp_err_t (*connect)(const printer_connection_config_t *, void *);
  esp_err_t (*subscribe)(const char *, void *);
  printer_dispatch_result_t (*publish_live)(const char *, const char *, size_t,
                                            int, bool, uint64_t, uint32_t,
                                            void *);
  void (*abort)(void *);
  uint64_t (*now_ms)(void *);
  void *ctx;
} printer_link_ops_t;
esp_err_t printer_link_start(void);
esp_err_t printer_link_start_with_ops(const printer_link_ops_t *);
esp_err_t printer_link_configure(const printer_connection_config_t *);
void printer_link_tick(uint64_t now_ms);
void printer_link_snapshot(printer_observation_t *);
/* Immediate invalidation; resource teardown is acknowledged by quiescent after
 * tick. */
void printer_link_suspend(bool);
bool printer_link_quiescent(void);
/* Expected history belongs to the armed cycle, never a newer projection. */
printer_dispatch_result_t
printer_link_resume_once(uint64_t cycle_id,
                         const printer_job_key_t *expected_job,
                         const printer_safety_history_t *expected_history,
                         uint32_t expected_epoch, uint64_t deadline_ms);
void printer_link_cancel_resume(void);
bool printer_link_connection_live(uint32_t epoch);
/* Native dispatcher rechecks the runtime after every wait; missing guard denies
 * Resume. */
void printer_link_set_dispatch_guard(bool (*guard)(void *), void *ctx);
