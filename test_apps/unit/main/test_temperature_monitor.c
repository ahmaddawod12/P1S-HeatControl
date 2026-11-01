#include "unity.h"

#include "temperature_monitor.h"

TEST_CASE("temperature median rejects one spike", "[temperature]")
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 1.5f);
    const float values[] = { 40.0f, 40.2f, 90.0f, 40.1f, 39.9f };

    for (size_t i = 0; i < 5U; ++i) {
        temperature_monitor_push(&monitor,
                                 values[i],
                                 TEMPERATURE_SENSOR_FAULT_NONE,
                                 500U * i);
    }

    const temperature_sample_t sample =
        temperature_monitor_snapshot(&monitor, 2000U);
    TEST_ASSERT_TRUE(sample.valid);
    TEST_ASSERT_EQUAL_UINT8(5U, sample.consecutive_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 41.4f, sample.raw_c);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 41.6f, sample.filtered_c);
}

TEST_CASE("temperature sample is stale after two seconds", "[temperature]")
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    temperature_monitor_push(&monitor,
                             25.0f,
                             TEMPERATURE_SENSOR_FAULT_NONE,
                             1000U);

    const temperature_sample_t sample =
        temperature_monitor_snapshot(&monitor, 3001U);
    TEST_ASSERT_FALSE(sample.valid);
    TEST_ASSERT_EQUAL(HEAT_FAULT_SENSOR_STALE, sample.fault);
}

TEST_CASE("sensor fault resets valid sample streak", "[temperature]")
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    temperature_monitor_push(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 0U);
    temperature_monitor_push(&monitor, 25.1f, TEMPERATURE_SENSOR_FAULT_NONE, 500U);
    temperature_monitor_push(&monitor, 0.0f, TEMPERATURE_SENSOR_FAULT_OPEN, 1000U);

    temperature_sample_t sample =
        temperature_monitor_snapshot(&monitor, 1000U);
    TEST_ASSERT_FALSE(sample.valid);
    TEST_ASSERT_EQUAL_UINT8(0U, sample.consecutive_valid);
    TEST_ASSERT_EQUAL(HEAT_FAULT_SENSOR_OPEN, sample.fault);

    temperature_monitor_push(&monitor, 25.2f, TEMPERATURE_SENSOR_FAULT_NONE, 1500U);
    temperature_monitor_push(&monitor, 25.3f, TEMPERATURE_SENSOR_FAULT_NONE, 2000U);
    temperature_monitor_push(&monitor, 25.4f, TEMPERATURE_SENSOR_FAULT_NONE, 2500U);
    sample = temperature_monitor_snapshot(&monitor, 2500U);
    TEST_ASSERT_TRUE(sample.valid);
    TEST_ASSERT_EQUAL_UINT8(3U, sample.consecutive_valid);
    TEST_ASSERT_EQUAL(HEAT_FAULT_NONE, sample.fault);
}
