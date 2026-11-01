#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "heat_controller.h"

static temperature_sample_t sample(float raw_c,
                                   float filtered_c,
                                   bool valid,
                                   uint8_t consecutive_valid,
                                   heat_fault_t fault)
{
    return (temperature_sample_t) {
        .raw_c = raw_c,
        .filtered_c = filtered_c,
        .valid = valid,
        .consecutive_valid = consecutive_valid,
        .timestamp_ms = 0U,
        .fault = fault,
    };
}

static heat_controller_t ready_controller(void)
{
    heat_controller_t controller;
    const heat_controller_config_t config = {
        .target_c = 60.0f,
        .hysteresis_c = 1.0f,
        .overtemp_c = 82.0f,
        .min_transition_ms = 5000U,
    };
    heat_controller_init(&controller, &config);
    const heat_controller_command_t ready = {
        .kind = HEAT_COMMAND_DISABLE,
    };
    assert(heat_controller_command(&controller, &ready, NULL, 0U));
    return controller;
}

static void test_hysteresis(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading =
        sample(57.0f, 57.0f, true, 3U, HEAT_FAULT_NONE);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    assert(heat_controller_command(&controller, &enable, &reading, 0U));
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HEATING);
    assert(heat_controller_snapshot(&controller).output_on);

    reading = sample(60.0f, 60.0f, true, 4U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &reading, 6000U);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HOLDING);
    assert(!heat_controller_snapshot(&controller).output_on);

    reading = sample(59.1f, 59.1f, true, 5U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &reading, 12000U);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HOLDING);

    reading = sample(59.0f, 59.0f, true, 6U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &reading, 18000U);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HEATING);
    assert(heat_controller_snapshot(&controller).output_on);
}

static void test_target_zero_disables(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading =
        sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    assert(heat_controller_command(&controller, &enable, &reading, 0U));

    const heat_controller_command_t target_zero = {
        .kind = HEAT_COMMAND_SET_TARGET,
        .target_c = 0.0f,
    };
    assert(heat_controller_command(&controller,
                                   &target_zero,
                                   &reading,
                                   1000U));
    const heat_controller_snapshot_t status =
        heat_controller_snapshot(&controller);
    assert(status.state == HEAT_STATE_IDLE);
    assert(!status.enabled);
    assert(!status.output_on);
}

static void test_faults_bypass_minimum_interval(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading =
        sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    assert(heat_controller_command(&controller, &enable, &reading, 0U));

    reading = sample(NAN,
                     NAN,
                     false,
                     0U,
                     HEAT_FAULT_SENSOR_OPEN);
    heat_controller_step(&controller, &reading, 1000U);
    heat_controller_snapshot_t status = heat_controller_snapshot(&controller);
    assert(status.state == HEAT_STATE_FAULT);
    assert(status.fault == HEAT_FAULT_SENSOR_OPEN);
    assert(!status.output_on);
    assert(!status.enabled);

    controller = ready_controller();
    reading = sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    assert(heat_controller_command(&controller, &enable, &reading, 0U));
    reading = sample(82.0f, 50.0f, true, 4U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &reading, 1000U);
    status = heat_controller_snapshot(&controller);
    assert(status.state == HEAT_STATE_FAULT);
    assert(status.fault == HEAT_FAULT_OVERTEMP);
    assert(!status.output_on);
}

static void test_acknowledgement_never_reenables(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading =
        sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    assert(heat_controller_command(&controller, &enable, &reading, 0U));
    reading = sample(NAN,
                     NAN,
                     false,
                     0U,
                     HEAT_FAULT_SENSOR_SHORT);
    heat_controller_step(&controller, &reading, 1000U);

    const heat_controller_command_t acknowledge = {
        .kind = HEAT_COMMAND_ACK_FAULT,
    };
    assert(!heat_controller_command(&controller,
                                    &acknowledge,
                                    &reading,
                                    2000U));
    reading = sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    assert(heat_controller_command(&controller,
                                   &acknowledge,
                                   &reading,
                                   3000U));
    const heat_controller_snapshot_t status =
        heat_controller_snapshot(&controller);
    assert(status.state == HEAT_STATE_IDLE);
    assert(status.fault == HEAT_FAULT_NONE);
    assert(!status.enabled);
    assert(!status.output_on);
}

static void test_restart_respects_transition_interval(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading =
        sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    assert(heat_controller_command(&controller, &enable, &reading, 0U));

    reading = sample(60.0f, 60.0f, true, 4U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &reading, 1000U);
    assert(!heat_controller_snapshot(&controller).output_on);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HOLDING);

    reading = sample(59.0f, 59.0f, true, 5U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &reading, 2000U);
    assert(!heat_controller_snapshot(&controller).output_on);
    heat_controller_step(&controller, &reading, 6000U);
    assert(heat_controller_snapshot(&controller).output_on);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HEATING);
}

static void test_target_turns_heater_off_immediately(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading =
        sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    assert(heat_controller_command(&controller, &enable, &reading, 0U));

    reading = sample(60.0f, 60.0f, true, 4U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &reading, 1000U);

    const heat_controller_snapshot_t status =
        heat_controller_snapshot(&controller);
    assert(!status.output_on);
    assert(status.state == HEAT_STATE_HOLDING);
}

static void test_heater_runs_continuously_until_target(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading =
        sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    assert(heat_controller_command(&controller, &enable, &reading, 0U));

    heat_controller_step(&controller, &reading, 120000U);
    assert(heat_controller_snapshot(&controller).output_on);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HEATING);

    heat_controller_step(&controller, &reading, 600000U);
    assert(heat_controller_snapshot(&controller).output_on);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HEATING);
}

static void test_target_validation(void)
{
    assert(heat_target_is_valid(0.0f));
    assert(heat_target_is_valid(0.5f));
    assert(heat_target_is_valid(60.0f));
    assert(heat_target_is_valid(80.0f));
    assert(!heat_target_is_valid(-0.5f));
    assert(!heat_target_is_valid(60.2f));
    assert(!heat_target_is_valid(80.5f));
    assert(!heat_target_is_valid(NAN));
    assert(!heat_target_is_valid(INFINITY));
}

static void test_external_fault_policy(void)
{
    heat_controller_t controller = ready_controller();
    const heat_controller_command_t nvs_fault = {
        .kind = HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
        .fault = HEAT_FAULT_NVS,
    };
    assert(heat_controller_command(&controller, &nvs_fault, NULL, 0U));
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_FAULT);

    const heat_controller_command_t acknowledge = {
        .kind = HEAT_COMMAND_ACK_FAULT,
    };
    assert(heat_controller_command(&controller,
                                   &acknowledge,
                                   NULL,
                                   1000U));
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_IDLE);

    const heat_controller_command_t output_fault = {
        .kind = HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
        .fault = HEAT_FAULT_OUTPUT_INIT,
    };
    assert(heat_controller_command(&controller, &output_fault, NULL, 2000U));
    assert(!heat_controller_command(&controller,
                                    &acknowledge,
                                    NULL,
                                    3000U));
}

int main(void)
{
    test_hysteresis();
    test_target_zero_disables();
    test_faults_bypass_minimum_interval();
    test_acknowledgement_never_reenables();
    test_restart_respects_transition_interval();
    test_target_turns_heater_off_immediately();
    test_heater_runs_continuously_until_target();
    test_target_validation();
    test_external_fault_policy();
    puts("heat_controller: PASS");
    return 0;
}
