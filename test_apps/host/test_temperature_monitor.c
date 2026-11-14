#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "temperature_monitor.h"

static void assert_close(float expected, float actual, float tolerance)
{
    assert(fabsf(expected - actual) <= tolerance);
}

static void test_median_rejects_spike(void)
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
    assert(sample.valid);
    assert(sample.consecutive_valid == 5U);
    assert_close(41.4f, sample.raw_c, 0.01f);
    assert_close(41.6f, sample.filtered_c, 0.01f);
}

static void test_stale_after_two_seconds(void)
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    temperature_monitor_push(&monitor,
                             25.0f,
                             TEMPERATURE_SENSOR_FAULT_NONE,
                             1000U);

    const temperature_sample_t sample =
        temperature_monitor_snapshot(&monitor, 3001U);
    assert(!sample.valid);
    assert(sample.fault == HEAT_FAULT_SENSOR_STALE);
}

static void test_fault_resets_valid_streak(void)
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    temperature_monitor_push(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 0U);
    temperature_monitor_push(&monitor, 25.1f, TEMPERATURE_SENSOR_FAULT_NONE, 500U);
    temperature_monitor_push(&monitor, 0.0f, TEMPERATURE_SENSOR_FAULT_OPEN, 1000U);

    temperature_sample_t sample =
        temperature_monitor_snapshot(&monitor, 1000U);
    assert(!sample.valid);
    assert(sample.consecutive_valid == 0U);
    assert(sample.fault == HEAT_FAULT_SENSOR_OPEN);

    temperature_monitor_push(&monitor, 25.2f, TEMPERATURE_SENSOR_FAULT_NONE, 1500U);
    temperature_monitor_push(&monitor, 25.3f, TEMPERATURE_SENSOR_FAULT_NONE, 2000U);
    temperature_monitor_push(&monitor, 25.4f, TEMPERATURE_SENSOR_FAULT_NONE, 2500U);
    sample = temperature_monitor_snapshot(&monitor, 2500U);
    assert(sample.valid);
    assert(sample.consecutive_valid == 3U);
    assert(sample.fault == HEAT_FAULT_NONE);
}

static void test_sample_gap_restarts_streak_and_filter(void)
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    temperature_monitor_push(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 0U);
    temperature_monitor_push(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 500U);
    temperature_monitor_push(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 1000U);
    temperature_monitor_push(&monitor, 60.0f, TEMPERATURE_SENSOR_FAULT_NONE, 4001U);
    const temperature_sample_t sample = temperature_monitor_snapshot(&monitor, 4001U);
    assert(sample.valid && sample.consecutive_valid == 1U);
    assert_close(60.0f, sample.filtered_c, 0.001f);
}

static void test_live_calibration_rebases_history_and_future_samples(void)
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    for (unsigned i = 0; i < 5U; ++i) {
        temperature_monitor_push(&monitor, 40.0f, TEMPERATURE_SENSOR_FAULT_NONE, i * 500U);
    }
    assert(temperature_monitor_set_calibration(&monitor, 2.5f));
    temperature_sample_t sample = temperature_monitor_snapshot(&monitor, 2000U);
    assert(sample.valid && sample.consecutive_valid == 5U && sample.timestamp_ms == 2000U);
    assert_close(42.5f, sample.raw_c, 0.001f);
    assert_close(42.5f, sample.filtered_c, 0.001f);

    assert(temperature_monitor_set_calibration(&monitor, -1.5f));
    temperature_monitor_push(&monitor, 40.0f, TEMPERATURE_SENSOR_FAULT_NONE, 2500U);
    sample = temperature_monitor_snapshot(&monitor, 2500U);
    assert_close(38.5f, sample.raw_c, 0.001f);
    assert_close(38.5f, sample.filtered_c, 0.001f);
    assert(!temperature_monitor_set_calibration(&monitor, NAN));
    assert_close(38.5f, temperature_monitor_snapshot(&monitor, 2500U).filtered_c, 0.001f);
}

static void test_live_calibration_preserves_fault_and_sample_age(void)
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    temperature_monitor_push(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 1000U);
    assert(temperature_monitor_set_calibration(&monitor, -2.0f));
    temperature_sample_t sample = temperature_monitor_snapshot(&monitor, 3001U);
    assert(!sample.valid && sample.fault == HEAT_FAULT_SENSOR_STALE);
    assert(sample.timestamp_ms == 1000U && sample.consecutive_valid == 0U);
    temperature_monitor_push(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_OPEN, 3500U);
    assert(temperature_monitor_set_calibration(&monitor, 1.0f));
    sample = temperature_monitor_snapshot(&monitor, 3500U);
    assert(!sample.valid && sample.fault == HEAT_FAULT_SENSOR_OPEN && isnan(sample.raw_c));
}

static void test_failed_acquisitions_reset_on_one_success_and_do_not_wrap(void)
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    temperature_monitor_push(&monitor, 25.0f, TEMPERATURE_SENSOR_FAULT_NONE, 0U);
    assert(temperature_monitor_snapshot(&monitor, 0U).consecutive_failures == 0U);
    for (unsigned i = 1U; i <= 4U; ++i) {
        temperature_monitor_push(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_SHORT, i * 1000U);
        for (unsigned tick = 0; tick < 10U; ++tick) {
            const temperature_sample_t bad = temperature_monitor_snapshot(&monitor, i * 1000U + tick * 100U);
            assert(!bad.valid && bad.consecutive_failures == i && bad.consecutive_valid == 0U);
        }
    }
    temperature_monitor_push(&monitor, 26.0f, TEMPERATURE_SENSOR_FAULT_NONE, 5000U);
    temperature_sample_t good = temperature_monitor_snapshot(&monitor, 5000U);
    assert(good.valid && good.consecutive_failures == 0U && good.consecutive_valid == 1U);
    assert_close(26.0f, good.filtered_c, 0.001f);
    for (unsigned i = 1U; i <= 260U; ++i) {
        /* NaN is a failed read even if transport reported success. */
        temperature_monitor_push(&monitor, NAN, TEMPERATURE_SENSOR_FAULT_NONE, 5000U + i * 1000U);
        const temperature_sample_t bad = temperature_monitor_snapshot(&monitor, 5000U + i * 1000U);
        assert(bad.consecutive_failures == (i < 5U ? i : 5U));
        assert(bad.fault == HEAT_FAULT_SENSOR_INVALID);
    }
    temperature_monitor_push(&monitor, 27.0f, TEMPERATURE_SENSOR_FAULT_NONE, 266000U);
    good = temperature_monitor_snapshot(&monitor, 266000U);
    assert(good.valid && good.consecutive_failures == 0U && good.consecutive_valid == 1U);
}

int main(void)
{
    test_failed_acquisitions_reset_on_one_success_and_do_not_wrap();
    test_live_calibration_rebases_history_and_future_samples();
    test_live_calibration_preserves_fault_and_sample_age();
    test_sample_gap_restarts_streak_and_filter();
    test_median_rejects_spike();
    test_stale_after_two_seconds();
    test_fault_resets_valid_streak();
    puts("temperature_monitor: PASS");
    return 0;
}
