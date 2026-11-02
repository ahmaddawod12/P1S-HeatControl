#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {
  RUNTIME_AUTO_NONE,
  RUNTIME_AUTO_OPERATOR_STOP,
  RUNTIME_AUTO_EXPIRED,
  RUNTIME_AUTO_FAULT,
  RUNTIME_AUTO_MAINTENANCE
} runtime_auto_end_reason_t;
typedef struct {
  bool active, teardown_applied;
  uint64_t cycle_id, expires_ms, last_ended_id;
  runtime_auto_end_reason_t reason;
} runtime_auto_status_t;
