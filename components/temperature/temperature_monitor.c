#include "temperature_monitor.h"

#include <math.h>
#include <string.h>

static heat_fault_t map_sensor_fault(temperature_sensor_fault_t sensor_fault)
{
    switch (sensor_fault) {
    case TEMPERATURE_SENSOR_FAULT_NONE:
        return HEAT_FAULT_NONE;
    case TEMPERATURE_SENSOR_FAULT_OPEN:
        return HEAT_FAULT_SENSOR_OPEN;
    case TEMPERATURE_SENSOR_FAULT_SHORT:
        return HEAT_FAULT_SENSOR_SHORT;
    case TEMPERATURE_SENSOR_FAULT_RANGE:
        return HEAT_FAULT_SENSOR_RANGE;
    case TEMPERATURE_SENSOR_FAULT_IO:
    default:
        return HEAT_FAULT_SENSOR_INVALID;
    }
}

static float median(const temperature_monitor_t *monitor)
{
    if (monitor->count == 0U) {
        return NAN;
    }

    float sorted[TEMPERATURE_MONITOR_WINDOW_SIZE];
    memcpy(sorted, monitor->window, monitor->count * sizeof(sorted[0]));
    for (size_t i = 1U; i < monitor->count; ++i) {
        const float value = sorted[i];
        size_t position = i;
        while (position > 0U && sorted[position - 1U] > value) {
            sorted[position] = sorted[position - 1U];
            --position;
        }
        sorted[position] = value;
    }

    const size_t middle = monitor->count / 2U;
    if ((monitor->count % 2U) != 0U) {
        return sorted[middle];
    }
    return (sorted[middle - 1U] + sorted[middle]) / 2.0f;
}

void temperature_monitor_init(temperature_monitor_t *monitor,
                              float calibration_offset_c)
{
    if (monitor == NULL) {
        return;
    }
    memset(monitor, 0, sizeof(*monitor));
    monitor->calibration_offset_c = isfinite(calibration_offset_c)
        ? calibration_offset_c
        : 0.0f;
    monitor->last_raw_c = NAN;
    monitor->fault = HEAT_FAULT_SENSOR_INVALID;
}

bool temperature_monitor_set_calibration(temperature_monitor_t *monitor,
                                         float calibration_offset_c)
{
    if (monitor == NULL || !isfinite(calibration_offset_c)) {
        return false;
    }
    const float delta = calibration_offset_c - monitor->calibration_offset_c;
    for (size_t i = 0U; i < monitor->count; ++i) {
        monitor->window[i] += delta;
    }
    monitor->last_raw_c += delta;
    monitor->calibration_offset_c = calibration_offset_c;
    return true;
}

void temperature_monitor_push(temperature_monitor_t *monitor,
                              float raw_temperature_c,
                              temperature_sensor_fault_t sensor_fault,
                              uint64_t timestamp_ms)
{
    if (monitor == NULL) {
        return;
    }

    if (monitor->has_sample &&
        timestamp_ms - monitor->last_timestamp_ms > TEMPERATURE_SAMPLE_MAX_AGE_MS) {
        /* Samples from before an acquisition stall cannot qualify recovery
         * or influence the first fresh median. */
        monitor->consecutive_valid = 0U;
        monitor->count = 0U;
        monitor->next = 0U;
    }
    monitor->has_sample = true;
    monitor->last_timestamp_ms = timestamp_ms;
    monitor->fault = map_sensor_fault(sensor_fault);
    if (monitor->fault != HEAT_FAULT_NONE ||
        !isfinite(raw_temperature_c)) {
        if (monitor->fault == HEAT_FAULT_NONE) {
            monitor->fault = HEAT_FAULT_SENSOR_INVALID;
        }
        monitor->consecutive_valid = 0U;
        monitor->count = 0U;
        monitor->next = 0U;
        monitor->last_raw_c = NAN;
        return;
    }

    const float calibrated =
        raw_temperature_c + monitor->calibration_offset_c;
    if (!isfinite(calibrated)) {
        monitor->fault = HEAT_FAULT_SENSOR_INVALID;
        monitor->consecutive_valid = 0U;
        monitor->count = 0U;
        monitor->next = 0U;
        monitor->last_raw_c = NAN;
        return;
    }

    monitor->last_raw_c = calibrated;
    monitor->window[monitor->next] = calibrated;
    monitor->next =
        (monitor->next + 1U) % TEMPERATURE_MONITOR_WINDOW_SIZE;
    if (monitor->count < TEMPERATURE_MONITOR_WINDOW_SIZE) {
        ++monitor->count;
    }
    if (monitor->consecutive_valid < UINT8_MAX) {
        ++monitor->consecutive_valid;
    }
}

temperature_sample_t temperature_monitor_snapshot(
    temperature_monitor_t *monitor,
    uint64_t now_ms)
{
    temperature_sample_t sample = {
        .raw_c = NAN,
        .filtered_c = NAN,
        .valid = false,
        .consecutive_valid = 0U,
        .timestamp_ms = 0U,
        .fault = HEAT_FAULT_SENSOR_INVALID,
    };
    if (monitor == NULL) {
        return sample;
    }

    sample.raw_c = monitor->last_raw_c;
    sample.filtered_c = median(monitor);
    sample.consecutive_valid = monitor->consecutive_valid;
    sample.timestamp_ms = monitor->last_timestamp_ms;
    sample.fault = monitor->fault;

    if (!monitor->has_sample) {
        return sample;
    }
    if (monitor->fault != HEAT_FAULT_NONE) {
        return sample;
    }
    if ((now_ms - monitor->last_timestamp_ms) > TEMPERATURE_SAMPLE_MAX_AGE_MS) {
        monitor->fault = HEAT_FAULT_SENSOR_STALE;
        monitor->consecutive_valid = 0U;
        sample.fault = HEAT_FAULT_SENSOR_STALE;
        sample.consecutive_valid = 0U;
        return sample;
    }

    sample.valid = true;
    return sample;
}
