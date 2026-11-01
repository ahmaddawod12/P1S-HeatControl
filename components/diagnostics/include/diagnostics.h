#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app_types.h"

#define DIAGNOSTICS_CAPACITY 32U

typedef struct {
    uint64_t timestamp_ms;
    heat_state_t from_state;
    heat_state_t to_state;
    heat_fault_t fault;
} diagnostic_record_t;

/* Allocation-free: safe to call from the controller task on every
 * transition. */
void diagnostics_record(uint64_t timestamp_ms,
                        heat_state_t from_state,
                        heat_state_t to_state,
                        heat_fault_t fault);

/* Copies up to `capacity` of the newest records, oldest first. */
void diagnostics_snapshot(diagnostic_record_t *out_records,
                          size_t capacity,
                          size_t *out_count);

void diagnostics_reset(void);
