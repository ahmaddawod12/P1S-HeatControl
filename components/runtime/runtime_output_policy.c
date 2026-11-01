#include "runtime_output_policy.h"

#include <stddef.h>

#define TARGET_FAN_COOLDOWN_MS 5000U
#define DISABLE_FAN_COOLDOWN_MS 10000U

void runtime_output_policy_init(runtime_output_policy_t *policy)
{
    if (policy != NULL) {
        policy->fan_cooldown_deadline_ms = 0U;
        policy->was_enabled = false;
        policy->maintenance = false;
    }
}

void runtime_output_policy_set_maintenance(runtime_output_policy_t *policy, bool enabled)
{
    if (policy != NULL) policy->maintenance = enabled;
}

runtime_output_request_t runtime_output_policy_step(
    runtime_output_policy_t *policy,
    const heat_controller_snapshot_t *snapshot,
    bool heater_is_on,
    bool manual_fan_on,
    uint64_t now_ms)
{
    if (policy == NULL || snapshot == NULL) {
        return (runtime_output_request_t) {0};
    }

    const bool operator_disabled =
        policy->was_enabled && !snapshot->enabled &&
        snapshot->state == HEAT_STATE_IDLE;
    policy->was_enabled = snapshot->enabled;

    if (operator_disabled) {
        policy->fan_cooldown_deadline_ms =
            now_ms + DISABLE_FAN_COOLDOWN_MS;
    } else if (snapshot->state != HEAT_STATE_HOLDING &&
               snapshot->state != HEAT_STATE_IDLE) {
        policy->fan_cooldown_deadline_ms = 0U;
    } else if (snapshot->state == HEAT_STATE_HOLDING && heater_is_on &&
               !snapshot->output_on) {
        policy->fan_cooldown_deadline_ms = now_ms + TARGET_FAN_COOLDOWN_MS;
    } else if (policy->fan_cooldown_deadline_ms != 0U &&
               now_ms >= policy->fan_cooldown_deadline_ms) {
        policy->fan_cooldown_deadline_ms = 0U;
    }

    const bool cooldown_active =
        policy->fan_cooldown_deadline_ms != 0U;
    return (runtime_output_request_t) {
        .heater_on = snapshot->output_on && !policy->maintenance,
        .fan_on = snapshot->output_on || cooldown_active || (manual_fan_on && !policy->maintenance),
    };
}
