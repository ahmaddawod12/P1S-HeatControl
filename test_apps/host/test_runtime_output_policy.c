#include <assert.h>
#include <stdio.h>

#include "runtime_output_policy.h"

static heat_controller_snapshot_t snapshot(heat_state_t state,
                                           bool enabled,
                                           bool heater_on)
{
    return (heat_controller_snapshot_t) {
        .state = state,
        .fault = HEAT_FAULT_NONE,
        .enabled = enabled,
        .output_on = heater_on,
        .target_c = 60.0f,
    };
}

static void test_target_reached_starts_five_second_fan_cooldown(void)
{
    runtime_output_policy_t policy;
    runtime_output_policy_init(&policy);

    heat_controller_snapshot_t status =
        snapshot(HEAT_STATE_HEATING, true, true);
    runtime_output_request_t request =
        runtime_output_policy_step(&policy, &status, false, false, 0U);
    assert(request.heater_on);
    assert(request.fan_on);

    status = snapshot(HEAT_STATE_HOLDING, true, false);
    request = runtime_output_policy_step(&policy, &status, true, false, 1000U);
    assert(!request.heater_on);
    assert(request.fan_on);

    request = runtime_output_policy_step(&policy, &status, false, false,
                                         5999U);
    assert(request.fan_on);

    request = runtime_output_policy_step(&policy, &status, false, false,
                                         6000U);
    assert(!request.fan_on);
}

static void test_disable_starts_ten_second_fan_cooldown(void)
{
    runtime_output_policy_t policy;
    runtime_output_policy_init(&policy);

    heat_controller_snapshot_t status =
        snapshot(HEAT_STATE_HEATING, true, true);
    (void) runtime_output_policy_step(&policy, &status, false, false, 0U);

    status = snapshot(HEAT_STATE_IDLE, false, false);
    runtime_output_request_t request =
        runtime_output_policy_step(&policy, &status, true, false, 1000U);
    assert(!request.heater_on);
    assert(request.fan_on);

    request = runtime_output_policy_step(&policy, &status, false, false,
                                         10999U);
    assert(request.fan_on);

    request = runtime_output_policy_step(&policy, &status, false, false,
                                         11000U);
    assert(!request.fan_on);
}

static void test_disable_from_holding_starts_ten_second_fan_cooldown(void)
{
    runtime_output_policy_t policy;
    runtime_output_policy_init(&policy);

    heat_controller_snapshot_t status =
        snapshot(HEAT_STATE_HOLDING, true, false);
    (void) runtime_output_policy_step(&policy, &status, false, false, 0U);

    status = snapshot(HEAT_STATE_IDLE, false, false);
    runtime_output_request_t request =
        runtime_output_policy_step(&policy, &status, false, false, 1000U);
    assert(!request.heater_on);
    assert(request.fan_on);

    request = runtime_output_policy_step(&policy, &status, false, false,
                                         11000U);
    assert(!request.fan_on);
}

static void test_fault_does_not_start_fan_cooldown(void)
{
    runtime_output_policy_t policy;
    runtime_output_policy_init(&policy);

    heat_controller_snapshot_t status =
        snapshot(HEAT_STATE_HEATING, true, true);
    (void) runtime_output_policy_step(&policy, &status, false, false, 0U);

    status = snapshot(HEAT_STATE_FAULT, false, false);
    status.fault = HEAT_FAULT_SENSOR_OPEN;
    const runtime_output_request_t request =
        runtime_output_policy_step(&policy, &status, true, false, 1000U);
    assert(!request.heater_on);
    assert(!request.fan_on);
}

static void test_manual_fan_remains_an_independent_override(void)
{
    runtime_output_policy_t policy;
    runtime_output_policy_init(&policy);
    const heat_controller_snapshot_t status =
        snapshot(HEAT_STATE_IDLE, false, false);

    const runtime_output_request_t request =
        runtime_output_policy_step(&policy, &status, false, true, 0U);
    assert(!request.heater_on);
    assert(request.fan_on);
}

static void test_maintenance_blocks_heat_and_manual_fan(void)
{
    runtime_output_policy_t policy;
    runtime_output_policy_init(&policy);
    runtime_output_policy_set_maintenance(&policy, true);
    heat_controller_snapshot_t status = snapshot(HEAT_STATE_HEATING, true, true);
    runtime_output_request_t request = runtime_output_policy_step(&policy, &status, true, true, 0);
    assert(!request.heater_on);
    status = snapshot(HEAT_STATE_IDLE, false, false);
    request = runtime_output_policy_step(&policy, &status, true, true, 1000);
    assert(!request.heater_on && request.fan_on);
    request = runtime_output_policy_step(&policy, &status, false, true, 10999);
    assert(!request.heater_on && request.fan_on);
    request = runtime_output_policy_step(&policy, &status, false, true, 11000);
    assert(!request.heater_on && !request.fan_on);
}

int main(void)
{
    test_target_reached_starts_five_second_fan_cooldown();
    test_disable_starts_ten_second_fan_cooldown();
    test_disable_from_holding_starts_ten_second_fan_cooldown();
    test_fault_does_not_start_fan_cooldown();
    test_manual_fan_remains_an_independent_override();
    test_maintenance_blocks_heat_and_manual_fan();
    puts("runtime_output_policy: PASS");
    return 0;
}
