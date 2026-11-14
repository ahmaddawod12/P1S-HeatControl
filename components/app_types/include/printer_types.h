#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRINTER_REPORT_MAX 32768U
typedef enum {
  PRINTER_RUN_UNKNOWN,
  PRINTER_RUN_IDLE,
  PRINTER_RUN_PREPARING,
  PRINTER_RUN_RUNNING,
  PRINTER_RUN_PAUSED,
  PRINTER_RUN_FINISHED,
  PRINTER_RUN_FAILED
} printer_run_state_t;
typedef struct {
  bool enabled;
  char ipv4[16], serial[33], access_code[65];
} printer_connection_config_t;
/* What Monitor P1S needs from the printer connection. */
typedef struct {
  bool connected, state_known;
  printer_run_state_t state;
  uint64_t received_ms; /* last push_status report of any content; 0 = none */
  uint32_t reconnects, connection_epoch;
  /* Monotonic across reconnects; retain starts and active -> terminal
   * transitions even when later reports hide the current state before a poll. */
  uint64_t started_prints, completed_prints;
  printer_run_state_t last_completed_state;
} printer_link_status_t;
typedef enum {
  PRINTER_MONITOR_OFF,
  PRINTER_MONITOR_WAITING,
  PRINTER_MONITOR_WATCHING
} printer_monitor_phase_t;
typedef enum {
  PRINTER_HOLD_NONE,
  PRINTER_HOLD_PAUSED,
  PRINTER_HOLD_DISCONNECTED,
  PRINTER_HOLD_UNKNOWN
} printer_hold_t;
typedef enum {
  PRINTER_MONITOR_END_NONE,
  PRINTER_MONITOR_END_FINISHED,
  PRINTER_MONITOR_END_FAILED,
  PRINTER_MONITOR_END_IDLE,
  PRINTER_MONITOR_END_NO_PRINT,
  PRINTER_MONITOR_END_STOPPED,
  PRINTER_MONITOR_END_HEATING_OFF,
  PRINTER_MONITOR_END_SETTINGS
} printer_monitor_end_t;
typedef struct {
  printer_monitor_phase_t phase;
  printer_hold_t hold;
  printer_monitor_end_t last_end;
  uint64_t started_ms;
  uint64_t started_prints_at_start;
  uint64_t completed_prints_at_start;
} printer_monitor_status_t;
typedef struct {
  printer_connection_config_t value;
  bool access_code_present;
} printer_config_request_t;
/* What one push_status report says about the print state. */
typedef struct {
  bool has_state;
  printer_run_state_t state;
} printer_report_t;
typedef struct {
  bool configured, enabled, access_code_set, applying, can_monitor;
  char ipv4[16], serial[33], reason[48], config_result[32];
  printer_link_status_t link;
  printer_monitor_status_t monitor;
  uint64_t wait_remaining_ms;
} printer_control_status_t;
