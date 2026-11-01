#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "heat_controller.h"

typedef struct {
    uint64_t fan_cooldown_deadline_ms;
    bool was_enabled;
    bool maintenance;
} runtime_output_policy_t;

typedef struct {
    bool heater_on;
    bool fan_on;
} runtime_output_request_t;

void runtime_output_policy_init(runtime_output_policy_t *policy);
void runtime_output_policy_set_maintenance(runtime_output_policy_t *policy, bool enabled);

runtime_output_request_t runtime_output_policy_step(
    runtime_output_policy_t *policy,
    const heat_controller_snapshot_t *snapshot,
    bool heater_is_on,
    bool manual_fan_on,
    uint64_t now_ms);
