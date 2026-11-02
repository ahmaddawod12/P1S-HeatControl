#pragma once
#include "app_types.h"
#include "esp_err.h"
#include "printer_types.h"
#include "runtime_auto_types.h"
typedef struct {
  printer_observation_t printer;
  app_status_t heat;
  runtime_auto_status_t automatic;
  uint64_t now_ms;
  bool maintenance, qualified;
} printer_cycle_input_t;
typedef struct {
  bool renew_heat, revoke_heat, send_resume;
} printer_cycle_effects_t;
typedef struct {
  printer_cycle_snapshot_t snapshot;
  printer_job_key_t offered_job;
  printer_safety_history_t offered_history, armed_history;
  float offered_target;
  uint64_t settle_since;
  bool reached_target, settling;
} printer_cycle_t;
void printer_cycle_init(printer_cycle_t *cycle);
/* Rechecked at the final send boundary, including after transport waits. */
bool printer_cycle_dispatch_ready(const app_status_t *heat, float target_c,
                                  uint64_t now_ms);
void printer_cycle_offer_arm(printer_cycle_t *cycle,
                             const printer_cycle_input_t *in,
                             const char fresh_token[33]);
esp_err_t printer_cycle_arm(printer_cycle_t *cycle,
                            const printer_arm_request_t *request,
                            const printer_cycle_input_t *in, uint64_t id);
void printer_cycle_step(printer_cycle_t *cycle, const printer_cycle_input_t *in,
                        printer_cycle_effects_t *out);
void printer_cycle_note_dispatch(printer_cycle_t *cycle,
                                 printer_dispatch_result_t result,
                                 uint64_t now);
void printer_cycle_stop(printer_cycle_t *cycle, printer_cycle_outcome_t reason,
                        uint64_t now);
void printer_cycle_snapshot(const printer_cycle_t *cycle,
                            printer_cycle_snapshot_t *out);
