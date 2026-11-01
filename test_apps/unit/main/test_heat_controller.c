#include <math.h>

#include "unity.h"

#include "heat_controller.h"

static temperature_sample_t valid_sample(float temperature_c)
{
    return (temperature_sample_t) {
        .raw_c = temperature_c,
        .filtered_c = temperature_c,
        .valid = true,
        .consecutive_valid = 3U,
        .fault = HEAT_FAULT_NONE,
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
    TEST_ASSERT_TRUE(heat_controller_command(&controller,
                                             &ready,
                                             NULL,
                                             0U));
    return controller;
}

TEST_CASE("controller applies one degree hysteresis", "[controller]")
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading = valid_sample(57.0f);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    TEST_ASSERT_TRUE(heat_controller_command(&controller,
                                             &enable,
                                             &reading,
                                             0U));
    TEST_ASSERT_TRUE(heat_controller_snapshot(&controller).output_on);

    reading = valid_sample(60.0f);
    heat_controller_step(&controller, &reading, 6000U);
    TEST_ASSERT_EQUAL(HEAT_STATE_HOLDING,
                      heat_controller_snapshot(&controller).state);
    reading = valid_sample(59.1f);
    heat_controller_step(&controller, &reading, 12000U);
    TEST_ASSERT_EQUAL(HEAT_STATE_HOLDING,
                      heat_controller_snapshot(&controller).state);
    reading = valid_sample(59.0f);
    heat_controller_step(&controller, &reading, 18000U);
    TEST_ASSERT_EQUAL(HEAT_STATE_HEATING,
                      heat_controller_snapshot(&controller).state);
}

TEST_CASE("controller fault bypasses relay transition interval",
          "[controller]")
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading = valid_sample(40.0f);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    TEST_ASSERT_TRUE(heat_controller_command(&controller,
                                             &enable,
                                             &reading,
                                             0U));
    reading = (temperature_sample_t) {
        .raw_c = NAN,
        .filtered_c = NAN,
        .valid = false,
        .fault = HEAT_FAULT_SENSOR_OPEN,
    };
    heat_controller_step(&controller, &reading, 1000U);
    const heat_controller_snapshot_t status =
        heat_controller_snapshot(&controller);
    TEST_ASSERT_EQUAL(HEAT_STATE_FAULT, status.state);
    TEST_ASSERT_EQUAL(HEAT_FAULT_SENSOR_OPEN, status.fault);
    TEST_ASSERT_FALSE(status.output_on);
}

TEST_CASE("controller validates half degree targets", "[controller]")
{
    TEST_ASSERT_TRUE(heat_target_is_valid(0.0f));
    TEST_ASSERT_TRUE(heat_target_is_valid(60.0f));
    TEST_ASSERT_TRUE(heat_target_is_valid(80.0f));
    TEST_ASSERT_FALSE(heat_target_is_valid(60.2f));
    TEST_ASSERT_FALSE(heat_target_is_valid(80.5f));
    TEST_ASSERT_FALSE(heat_target_is_valid(NAN));
}

TEST_CASE("controller heats continuously until target", "[controller]")
{
    heat_controller_t controller = ready_controller();
    temperature_sample_t reading = valid_sample(40.0f);
    const heat_controller_command_t enable = {
        .kind = HEAT_COMMAND_ENABLE,
    };
    TEST_ASSERT_TRUE(heat_controller_command(&controller,
                                             &enable,
                                             &reading,
                                             0U));

    heat_controller_step(&controller, &reading, 120000U);
    TEST_ASSERT_EQUAL(HEAT_STATE_HEATING,
                      heat_controller_snapshot(&controller).state);
    TEST_ASSERT_TRUE(heat_controller_snapshot(&controller).output_on);

    heat_controller_step(&controller, &reading, 600000U);
    TEST_ASSERT_EQUAL(HEAT_STATE_HEATING,
                      heat_controller_snapshot(&controller).state);
    TEST_ASSERT_TRUE(heat_controller_snapshot(&controller).output_on);

    reading = valid_sample(60.0f);
    heat_controller_step(&controller, &reading, 600001U);
    TEST_ASSERT_EQUAL(HEAT_STATE_HOLDING,
                      heat_controller_snapshot(&controller).state);
    TEST_ASSERT_FALSE(heat_controller_snapshot(&controller).output_on);
}
