#pragma once
#include "printer_report.h"
typedef struct {
  printer_report_delta_t values;
  uint32_t epoch, report_revision;
  uint64_t print_epoch, received_ms;
  printer_safety_history_t history;
  bool have_report, observed_terminal, observed_run;
  bool observed_pause, left_initial_pause;
} printer_status_t;
void printer_status_init(printer_status_t *status, uint32_t epoch);
bool printer_status_apply(printer_status_t *status,
                          const printer_report_delta_t *delta, uint32_t epoch,
                          uint64_t now_ms);
bool printer_status_baseline_valid(const printer_status_t *status);
bool printer_status_snapshot_required(const printer_status_t *status);
void printer_status_snapshot(const printer_status_t *status, uint64_t now_ms,
                             printer_observation_t *out);
bool printer_job_same(const printer_job_key_t *a, const printer_job_key_t *b);
