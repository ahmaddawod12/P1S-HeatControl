#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TEMPERATURE_SAMPLE_MAX_AGE_MS 2000U

typedef enum {
    HEAT_STATE_BOOT = 0,
    HEAT_STATE_IDLE,
    HEAT_STATE_HEATING,
    HEAT_STATE_HOLDING,
    HEAT_STATE_FAULT,
} heat_state_t;

typedef enum {
    HEAT_FAULT_NONE = 0,
    HEAT_FAULT_SENSOR_OPEN,
    HEAT_FAULT_SENSOR_SHORT,
    HEAT_FAULT_SENSOR_RANGE,
    HEAT_FAULT_SENSOR_INVALID,
    HEAT_FAULT_SENSOR_STALE,
    HEAT_FAULT_OVERTEMP,
    HEAT_FAULT_NVS,
    HEAT_FAULT_OUTPUT_INIT,
} heat_fault_t;

typedef struct {
    float raw_c;
    float filtered_c;
    bool valid;
    uint8_t consecutive_valid;
    uint64_t timestamp_ms;
    heat_fault_t fault;
} temperature_sample_t;

typedef struct {
    heat_state_t state;
    heat_fault_t fault;
    bool enabled;
    bool heater_on;
    bool fan_on;
    bool manual_fan_on;
    float target_c;
    temperature_sample_t temperature;
    int8_t wifi_rssi;
    char ip_addr[16];
    uint64_t uptime_ms;
} app_status_t;

static inline app_status_t app_status_initial(void)
{
    return (app_status_t) {
        .state = HEAT_STATE_BOOT,
        .fault = HEAT_FAULT_NONE,
        .enabled = false,
        .heater_on = false,
        .fan_on = false,
        .manual_fan_on = false,
        .target_c = 60.0f,
        .temperature = { .valid = false },
        .wifi_rssi = 0,
        .ip_addr = "0.0.0.0",
        .uptime_ms = 0,
    };
}
