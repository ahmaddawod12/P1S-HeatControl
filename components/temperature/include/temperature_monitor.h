#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_types.h"
#include "temperature_sensor_types.h"

#define TEMPERATURE_MONITOR_WINDOW_SIZE 5U

typedef struct {
    float window[TEMPERATURE_MONITOR_WINDOW_SIZE];
    size_t count;
    size_t next;
    float calibration_offset_c;
    float last_raw_c;
    uint8_t consecutive_valid;
    uint64_t last_timestamp_ms;
    heat_fault_t fault;
    bool has_sample;
} temperature_monitor_t;

void temperature_monitor_init(temperature_monitor_t *monitor,
                              float calibration_offset_c);
void temperature_monitor_push(temperature_monitor_t *monitor,
                              float raw_temperature_c,
                              temperature_sensor_fault_t sensor_fault,
                              uint64_t timestamp_ms);
temperature_sample_t temperature_monitor_snapshot(
    temperature_monitor_t *monitor,
    uint64_t now_ms);
