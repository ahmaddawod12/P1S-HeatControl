#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "heat_controller.h"
#include "temperature_monitor.h"

static temperature_sample_t sample(float raw_c,
                                   float filtered_c,
                                   bool valid,
                                   uint8_t consecutive_valid,
                                   heat_fault_t fault)
{
    return (temperature_sample_t) {
        .probe_c = raw_c,
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
    heat_controller_note_relay_off(&controller, 6000U);
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

static void test_configured_restart_threshold(void)
{
    heat_controller_t controller = ready_controller();
    heat_controller_command_t restart = {.kind = HEAT_COMMAND_SET_RESTART, .restart_c = 60.0f};
    assert(!heat_controller_command(&controller, &restart, NULL, 0U));
    restart.restart_c = 39.5f;
    assert(!heat_controller_command(&controller, &restart, NULL, 0U));
    restart.restart_c = 55.0f;
    assert(heat_controller_command(&controller, &restart, NULL, 0U));
    assert(heat_controller_snapshot(&controller).restart_c == 55.0f);
    assert(!heat_controller_snapshot(&controller).enabled);

    temperature_sample_t reading = sample(57.0f, 57.0f, true, 3U, HEAT_FAULT_NONE);
    const heat_controller_command_t enable = {.kind = HEAT_COMMAND_ENABLE};
    assert(heat_controller_command(&controller, &enable, &reading, 0U));

    reading = sample(60.0f, 60.0f, true, 4U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &reading, 6000U);
    heat_controller_note_relay_off(&controller, 6000U);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HOLDING);
    reading = sample(55.0f, 55.0f, true, 5U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &reading, 10999U);
    assert(!heat_controller_snapshot(&controller).output_on);
    reading.filtered_c = 55.125f;
    heat_controller_step(&controller, &reading, 11000U);
    assert(!heat_controller_snapshot(&controller).output_on);
    reading.filtered_c = 55.0f;
    heat_controller_step(&controller, &reading, 12000U);
    assert(heat_controller_snapshot(&controller).output_on);

    const heat_controller_command_t target = {.kind = HEAT_COMMAND_SET_TARGET, .target_c = 55.0f};
    assert(heat_controller_command(&controller, &target, &reading, 13000U));
    assert(heat_controller_snapshot(&controller).restart_c == 50.0f);
    assert(!heat_controller_snapshot(&controller).output_on);
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
    heat_controller_note_relay_off(&controller, 1000U);
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

static const heat_controller_command_t k_enable = {.kind = HEAT_COMMAND_ENABLE};

static void test_manual_reenable_respects_physical_off_interval(void)
{
    heat_controller_t controller = ready_controller();
    const heat_controller_command_t restart = {.kind = HEAT_COMMAND_SET_RESTART, .restart_c = 55.0f};
    const heat_controller_command_t disable = {.kind = HEAT_COMMAND_DISABLE};
    const temperature_sample_t reading = sample(58.0f, 58.0f, true, 3U, HEAT_FAULT_NONE);
    assert(heat_controller_command(&controller, &restart, NULL, 0U));
    assert(heat_controller_command(&controller, &k_enable, &reading, 0U));
    assert(heat_controller_snapshot(&controller).output_on);
    assert(heat_controller_command(&controller, &disable, NULL, 1000U));
    heat_controller_note_relay_off(&controller, 1050U);

    assert(heat_controller_command(&controller, &k_enable, &reading, 1200U));
    assert(heat_controller_snapshot(&controller).enabled);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HOLDING);
    heat_controller_step(&controller, &reading, 6049U);
    assert(!heat_controller_snapshot(&controller).output_on);
    heat_controller_step(&controller, &reading, 6050U);
    assert(heat_controller_snapshot(&controller).output_on);
}

static void test_restart_lockout_requires_a_cool_healthy_probe(void)
{
    heat_controller_t controller = ready_controller();
    const heat_controller_command_t lockout = {
        .kind = HEAT_COMMAND_LATCH_EXTERNAL_FAULT, .fault = HEAT_FAULT_RESTART_LOCKOUT,
    };
    const heat_controller_command_t ack = {.kind = HEAT_COMMAND_ACK_FAULT};
    const temperature_sample_t hot = sample(72.0f, 80.0f, true, 4U, HEAT_FAULT_NONE);
    const temperature_sample_t recovering = sample(40.0f, 40.0f, true, 2U, HEAT_FAULT_NONE);
    const temperature_sample_t healthy = sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    assert(heat_controller_command(&controller, &lockout, NULL, 0U));
    assert(!heat_controller_command(&controller, &k_enable, &healthy, 1200U));
    assert(!heat_controller_command(&controller, &ack, &hot, 1500U));
    assert(!heat_controller_command(&controller, &ack, &recovering, 2000U));
    assert(heat_controller_command(&controller, &ack, &healthy, 2500U));
    const heat_controller_snapshot_t status = heat_controller_snapshot(&controller);
    assert(status.fault == HEAT_FAULT_NONE && !status.enabled && !status.output_on);
}

static void test_output_init_fault_survives_later_storage_faults(void)
{
    const heat_controller_command_t output_fault = {
        .kind = HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
        .fault = HEAT_FAULT_OUTPUT_INIT,
    };
    const heat_controller_command_t nvs_fault = {
        .kind = HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
        .fault = HEAT_FAULT_NVS,
    };
    const heat_controller_command_t disable = {.kind = HEAT_COMMAND_DISABLE};
    const heat_controller_command_t target = {
        .kind = HEAT_COMMAND_SET_TARGET, .target_c = 50.0f,
    };
    const heat_controller_command_t acknowledge = {.kind = HEAT_COMMAND_ACK_FAULT};
    const temperature_sample_t cold = sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);

    for (unsigned nvs_first = 0U; nvs_first < 2U; ++nvs_first) {
        heat_controller_t controller = ready_controller();
        assert(heat_controller_command(&controller, &k_enable, &cold, 0U));
        if (nvs_first) {
            assert(heat_controller_command(&controller, &nvs_fault, &cold, 100U));
        }
        assert(heat_controller_command(&controller, &output_fault, &cold, 200U));
        assert(heat_controller_command(&controller, &nvs_fault, &cold, 300U));
        assert(heat_controller_command(&controller, &nvs_fault, &cold, 400U));
        assert(heat_controller_snapshot(&controller).fault == HEAT_FAULT_OUTPUT_INIT);
        assert(!heat_controller_command(&controller, &acknowledge, &cold, 500U));
        assert(heat_controller_command(&controller, &disable, &cold, 600U));
        assert(heat_controller_command(&controller, &target, &cold, 700U));
        assert(!heat_controller_command(&controller, &k_enable, &cold, 800U));
        heat_controller_step(&controller, &cold, 10000U);
        const heat_controller_snapshot_t status = heat_controller_snapshot(&controller);
        assert(status.state == HEAT_STATE_FAULT && status.fault == HEAT_FAULT_OUTPUT_INIT);
        assert(!status.enabled && !status.output_on);
    }
}

static void test_inhibit_holds_output_off(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t cold = sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    assert(heat_controller_command(&controller, &k_enable, &cold, 0U));
    assert(heat_controller_snapshot(&controller).output_on);

    heat_controller_set_inhibit(&controller, true);
    heat_controller_snapshot_t s = heat_controller_snapshot(&controller);
    assert(!s.output_on && s.enabled && s.inhibited);
    assert(s.state == HEAT_STATE_HOLDING);

    heat_controller_step(&controller, &cold, 60000U);
    s = heat_controller_snapshot(&controller);
    assert(!s.output_on && s.enabled && s.state == HEAT_STATE_HOLDING);
}

static void test_inhibit_release_waits_min_transition(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t cold = sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    assert(heat_controller_command(&controller, &k_enable, &cold, 0U));
    heat_controller_set_inhibit(&controller, true);
    heat_controller_note_relay_off(&controller, 1000U);
    heat_controller_set_inhibit(&controller, false);
    assert(!heat_controller_snapshot(&controller).inhibited);

    heat_controller_step(&controller, &cold, 5999U);
    assert(!heat_controller_snapshot(&controller).output_on);
    heat_controller_step(&controller, &cold, 6000U);
    assert(heat_controller_snapshot(&controller).output_on);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_HEATING);
}

static void test_enable_while_inhibited_stays_off(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t cold = sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    heat_controller_set_inhibit(&controller, true);
    assert(heat_controller_command(&controller, &k_enable, &cold, 100U));
    const heat_controller_snapshot_t s = heat_controller_snapshot(&controller);
    assert(s.enabled && !s.output_on && s.state == HEAT_STATE_HOLDING);
}

static void test_inhibit_does_not_mask_faults(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t cold = sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    assert(heat_controller_command(&controller, &k_enable, &cold, 0U));
    heat_controller_set_inhibit(&controller, true);
    temperature_sample_t hot = sample(85.0f, 85.0f, true, 4U, HEAT_FAULT_NONE);
    heat_controller_step(&controller, &hot, 2000U);
    const heat_controller_snapshot_t s = heat_controller_snapshot(&controller);
    assert(s.state == HEAT_STATE_FAULT && s.fault == HEAT_FAULT_OVERTEMP);
    assert(!s.enabled && !s.output_on);
}

static void test_disable_while_inhibited(void)
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t cold = sample(40.0f, 40.0f, true, 3U, HEAT_FAULT_NONE);
    assert(heat_controller_command(&controller, &k_enable, &cold, 0U));
    heat_controller_set_inhibit(&controller, true);
    const heat_controller_command_t disable = {.kind = HEAT_COMMAND_DISABLE};
    assert(heat_controller_command(&controller, &disable, &cold, 2000U));
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_IDLE);
    assert(!heat_controller_snapshot(&controller).enabled);
    heat_controller_set_inhibit(&controller, false);
    heat_controller_step(&controller, &cold, 60000U);
    assert(!heat_controller_snapshot(&controller).output_on);
}

static temperature_sample_t acquire(temperature_monitor_t *monitor, float value,
                                    temperature_sensor_fault_t fault, uint64_t time)
{
    temperature_monitor_push(monitor, value, fault, time);
    return temperature_monitor_snapshot(monitor, time);
}

static heat_controller_t start_with_monitor(temperature_monitor_t *monitor)
{
    temperature_monitor_init(monitor, 0.0f);
    temperature_sample_t reading = {0};
    for (unsigned i = 0; i < 3U; ++i) {
        reading = acquire(monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, i * 1000U);
    }
    heat_controller_t controller = ready_controller();
    assert(heat_controller_command(&controller, &k_enable, &reading, 2000U));
    return controller;
}

static void test_five_failed_acquisitions_latch_even_with_mixed_errors(void)
{
    temperature_monitor_t monitor;
    heat_controller_t controller = start_with_monitor(&monitor);
    const temperature_sensor_fault_t faults[] = {
        TEMPERATURE_SENSOR_FAULT_SHORT, TEMPERATURE_SENSOR_FAULT_OPEN,
        TEMPERATURE_SENSOR_FAULT_IO, TEMPERATURE_SENSOR_FAULT_RANGE,
        TEMPERATURE_SENSOR_FAULT_SHORT,
    };
    for (unsigned i = 0; i < 5U; ++i) {
        const uint64_t time = 3000U + i * 1000U;
        const temperature_sample_t reading = acquire(&monitor, NAN, faults[i], time);
        assert(!reading.valid);
        /* Repeated 100 ms control ticks see one acquisition, not new failures. */
        for (unsigned tick = 0; tick < 10U; ++tick) {
            heat_controller_step(&controller, &reading, time + tick * 100U);
            const heat_controller_snapshot_t status = heat_controller_snapshot(&controller);
            if (i < 4U) {
                assert(status.enabled && status.output_on);
                assert(status.state == HEAT_STATE_HEATING && status.fault == HEAT_FAULT_NONE);
            } else {
                assert(!status.enabled && !status.output_on);
                assert(status.state == HEAT_STATE_FAULT && status.fault == HEAT_FAULT_SENSOR_SHORT);
            }
        }
    }
    const temperature_sample_t good = acquire(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 8000U);
    heat_controller_step(&controller, &good, 8000U);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_FAULT);
}

static void test_one_success_resets_all_previous_failures(void)
{
    temperature_monitor_t monitor;
    heat_controller_t controller = start_with_monitor(&monitor);
    for (unsigned sequence = 0; sequence < 2U; ++sequence) {
        for (unsigned i = 0; i < 4U; ++i) {
            const uint64_t time = 3000U + sequence * 5000U + i * 1000U;
            const temperature_sample_t bad = acquire(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_SHORT, time);
            heat_controller_step(&controller, &bad, time);
            assert(heat_controller_snapshot(&controller).output_on);
            assert(heat_controller_snapshot(&controller).fault == HEAT_FAULT_NONE);
        }
        if (sequence == 0U) {
            const temperature_sample_t good = acquire(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 7000U);
            assert(good.valid && good.consecutive_valid == 1U);
            heat_controller_step(&controller, &good, 7000U);
            assert(heat_controller_snapshot(&controller).output_on);
        }
    }
    const temperature_sample_t fifth = acquire(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_SHORT, 12000U);
    heat_controller_step(&controller, &fifth, 12000U);
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_FAULT);
}

static void test_startup_retries_cannot_enable_heating(void)
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    heat_controller_t controller = ready_controller();
    for (unsigned i = 0; i < 5U; ++i) {
        const temperature_sample_t bad = acquire(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_OPEN, i * 1000U);
        heat_controller_step(&controller, &bad, i * 1000U);
        assert(!heat_controller_snapshot(&controller).output_on);
        assert(!heat_controller_command(&controller, &k_enable, &bad, i * 1000U));
        assert(heat_controller_snapshot(&controller).state ==
               (i < 4U ? HEAT_STATE_IDLE : HEAT_STATE_FAULT));
    }
}

static void test_hot_success_after_retries_still_trips_immediately(void)
{
    temperature_monitor_t monitor;
    heat_controller_t controller = start_with_monitor(&monitor);
    for (unsigned i = 0; i < 4U; ++i) {
        const temperature_sample_t bad = acquire(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_IO, 3000U + i * 1000U);
        heat_controller_step(&controller, &bad, 3000U + i * 1000U);
    }
    const temperature_sample_t hot = acquire(&monitor, 82.0f, TEMPERATURE_SENSOR_FAULT_NONE, 7000U);
    heat_controller_step(&controller, &hot, 7000U);
    const heat_controller_snapshot_t status = heat_controller_snapshot(&controller);
    assert(status.state == HEAT_STATE_FAULT && status.fault == HEAT_FAULT_OVERTEMP);
    assert(!status.enabled && !status.output_on);
}

static void test_negative_calibration_cannot_raise_safety_limits(void)
{
    temperature_monitor_t monitor;
    heat_controller_t controller = start_with_monitor(&monitor);
    const heat_controller_command_t acknowledge = {.kind = HEAT_COMMAND_ACK_FAULT};
    assert(temperature_monitor_set_calibration(&monitor, -10.0f));
    temperature_sample_t reading = acquire(&monitor, 82.0f, TEMPERATURE_SENSOR_FAULT_NONE, 3000U);
    assert(reading.probe_c == 82.0f && reading.raw_c == 72.0f && reading.filtered_c < 60.0f);
    heat_controller_step(&controller, &reading, 3000U);
    assert(heat_controller_snapshot(&controller).fault == HEAT_FAULT_OVERTEMP);
    assert(!heat_controller_snapshot(&controller).output_on);

    reading = acquire(&monitor, 80.0f, TEMPERATURE_SENSOR_FAULT_NONE, 4000U);
    assert(!heat_controller_command(&controller, &acknowledge, &reading, 4000U));
    reading = acquire(&monitor, 79.0f, TEMPERATURE_SENSOR_FAULT_NONE, 5000U);
    assert(heat_controller_command(&controller, &acknowledge, &reading, 5000U));
    assert(heat_controller_snapshot(&controller).state == HEAT_STATE_IDLE);
    assert(!heat_controller_snapshot(&controller).output_on);
}

static void test_restart_after_retries_requires_three_good_reads(void)
{
    temperature_monitor_t monitor;
    heat_controller_t controller = start_with_monitor(&monitor);
    heat_controller_set_inhibit(&controller, true);
    heat_controller_note_relay_off(&controller, 3000U);
    const temperature_sample_t bad = acquire(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_IO, 3000U);
    heat_controller_step(&controller, &bad, 3000U);
    heat_controller_set_inhibit(&controller, false);
    /* Keep acquisition fresh until after the 5000 ms restart delay. */
    for (unsigned i = 0; i < 3U; ++i) {
        const temperature_sample_t good = acquire(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 4000U + i * 1000U);
        heat_controller_step(&controller, &good, 4000U + i * 1000U);
        assert(!heat_controller_snapshot(&controller).output_on);
    }
    const temperature_sample_t retry = acquire(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_IO, 7000U);
    heat_controller_step(&controller, &retry, 7000U);
    for (unsigned i = 0; i < 3U; ++i) {
        const temperature_sample_t good = acquire(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 8000U + i * 1000U);
        heat_controller_step(&controller, &good, 8000U + i * 1000U);
        const heat_controller_snapshot_t status = heat_controller_snapshot(&controller);
        assert(status.enabled && status.fault == HEAT_FAULT_NONE);
        assert(status.output_on == (i == 2U));
    }
}

static void test_disable_and_staleness_bypass_retry_allowance(void)
{
    temperature_monitor_t monitor;
    heat_controller_t controller = start_with_monitor(&monitor);
    temperature_sample_t bad = acquire(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_SHORT, 3000U);
    heat_controller_step(&controller, &bad, 3000U);
    const heat_controller_command_t disable = {.kind = HEAT_COMMAND_DISABLE};
    assert(heat_controller_command(&controller, &disable, &bad, 3100U));
    assert(!heat_controller_snapshot(&controller).enabled && !heat_controller_snapshot(&controller).output_on);

    controller = start_with_monitor(&monitor);
    bad = acquire(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_SHORT, 3000U);
    heat_controller_step(&controller, &bad, 3000U);
    bad = temperature_monitor_snapshot(&monitor, 5001U);
    heat_controller_step(&controller, &bad, 5001U);
    const heat_controller_snapshot_t status = heat_controller_snapshot(&controller);
    assert(status.state == HEAT_STATE_FAULT && status.fault == HEAT_FAULT_SENSOR_STALE);
    assert(!status.output_on);
}

int main(void)
{
    test_output_init_fault_survives_later_storage_faults();
    test_five_failed_acquisitions_latch_even_with_mixed_errors();
    test_one_success_resets_all_previous_failures();
    test_startup_retries_cannot_enable_heating();
    test_hot_success_after_retries_still_trips_immediately();
    test_negative_calibration_cannot_raise_safety_limits();
    test_restart_after_retries_requires_three_good_reads();
    test_disable_and_staleness_bypass_retry_allowance();
    test_inhibit_holds_output_off();
    test_inhibit_release_waits_min_transition();
    test_enable_while_inhibited_stays_off();
    test_inhibit_does_not_mask_faults();
    test_disable_while_inhibited();
    test_hysteresis();
    test_configured_restart_threshold();
    test_manual_reenable_respects_physical_off_interval();
    test_restart_lockout_requires_a_cool_healthy_probe();
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
