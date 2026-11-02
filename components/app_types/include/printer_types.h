#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRINTER_REPORT_MAX 32768U
#define PRINTER_TELEMETRY_MS 30000U
typedef enum {
  PRINTER_RUN_UNKNOWN,
  PRINTER_RUN_IDLE,
  PRINTER_RUN_PREPARING,
  PRINTER_RUN_RUNNING,
  PRINTER_RUN_PAUSED,
  PRINTER_RUN_FINISHED,
  PRINTER_RUN_FAILED
} printer_run_state_t;
typedef enum {
  PRINTER_FIELD_STATE = 1U,
  PRINTER_FIELD_LAYER = 2U,
  PRINTER_FIELD_START = 4U,
  PRINTER_FIELD_TASK = 8U,
  PRINTER_FIELD_SUBTASK = 16U,
  PRINTER_FIELD_FILE = 32U,
  PRINTER_FIELD_LABEL = 64U,
  PRINTER_FIELD_TYPE = 128U,
  PRINTER_FIELD_PROGRESS = 256U,
  PRINTER_FIELD_REMAINING = 512U,
  PRINTER_FIELD_BED = 1024U,
  PRINTER_FIELD_NOZZLE = 2048U,
  PRINTER_FIELD_ERROR = 4096U,
  PRINTER_FIELD_HMS = 8192U
} printer_field_t;
typedef struct {
  bool enabled;
  char ipv4[16], serial[33], access_code[65], ntp_host[254];
} printer_connection_config_t;
typedef struct {
  printer_connection_config_t value;
  bool access_code_present;
} printer_config_request_t;
typedef struct {
  uint32_t present;
  printer_run_state_t state;
  uint32_t layer, error;
  uint64_t start_time;
  char task[65], subtask[65], file[257], label[97], print_type[33];
  int progress, remaining;
  float bed_c, nozzle_c;
  bool hms_blocking;
} printer_report_delta_t;
typedef struct {
  uint32_t connection_epoch;
  uint64_t print_epoch, start_time;
  bool identity_valid;
  char task[65], subtask[65], file[257], label[97];
} printer_job_key_t;

/* Monotonic within one observed run: later deltas cannot erase adverse events.
 */
typedef struct {
  uint64_t pause, error, identity;
} printer_safety_history_t;
typedef struct {
  printer_run_state_t state;
  printer_job_key_t job;
  printer_safety_history_t history;
  uint32_t connection_epoch;
  uint64_t received_ms;
  uint32_t valid_mask, report_revision;
  bool connected, fresh, baseline_valid, startup_eligible, blocking_error;
  int progress, remaining;
  bool progress_valid, remaining_valid;
  float bed_c, nozzle_c;
  char reason[48];
} printer_observation_t;
typedef struct {
  char arm_token[33];
  bool initial_pause_confirmed;
} printer_arm_request_t;
typedef enum {
  PRINTER_DISPATCH_REJECTED,
  PRINTER_DISPATCH_SENT,
  PRINTER_DISPATCH_UNKNOWN
} printer_dispatch_result_t;
typedef enum {
  PRINTER_CYCLE_READY,
  PRINTER_CYCLE_PREHEATING,
  PRINTER_CYCLE_RESUME_PENDING,
  PRINTER_CYCLE_PRINTING,
  PRINTER_CYCLE_COMPLETED,
  PRINTER_CYCLE_STOPPED,
  PRINTER_CYCLE_ERROR
} printer_cycle_state_t;
typedef enum {
  PRINTER_OUTCOME_NONE,
  PRINTER_OUTCOME_COMPLETED,
  PRINTER_OUTCOME_OPERATOR_STOP,
  PRINTER_OUTCOME_LATER_PAUSE,
  PRINTER_OUTCOME_CANCELLED,
  PRINTER_OUTCOME_PRINTER_ERROR,
  PRINTER_OUTCOME_IDENTITY_CHANGED,
  PRINTER_OUTCOME_TELEMETRY_STALE,
  PRINTER_OUTCOME_LINK_LOST,
  PRINTER_OUTCOME_HEAT_FAULT,
  PRINTER_OUTCOME_LEASE_EXPIRED,
  PRINTER_OUTCOME_PREHEAT_TIMEOUT,
  PRINTER_OUTCOME_RESUME_REJECTED,
  PRINTER_OUTCOME_RESUME_UNCERTAIN
} printer_cycle_outcome_t;
typedef struct {
  uint64_t cycle_id, deadline_ms, token_expires_ms;
  printer_cycle_state_t state;
  printer_cycle_outcome_t outcome;
  float target_c;
  printer_job_key_t job;
  uint32_t connection_epoch;
  bool resume_attempted;
  char arm_token[33];
} printer_cycle_snapshot_t;
typedef struct {
  bool configured, enabled, access_code_set, applying, can_arm;
  char ipv4[16], serial[33], ntp_host[254], reason[48], config_result[32];
  printer_observation_t observation;
  printer_cycle_snapshot_t cycle;
} printer_control_status_t;
