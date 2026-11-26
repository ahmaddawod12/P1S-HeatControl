#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TEMPERATURE_SAMPLE_MAX_AGE_MS 2000U
#define TEMPERATURE_SENSOR_FAILURE_LIMIT 5U

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
    HEAT_FAULT_RESTART_LOCKOUT,
} heat_fault_t;

/* Values 0 and 1 match the legacy saved heating toggle. */
typedef enum {
    HEAT_MODE_OFF = 0,
    HEAT_MODE_MANUAL = 1,
    HEAT_MODE_MONITOR_WAITING = 2,
    HEAT_MODE_MONITOR_WATCHING = 3,
    /* Durable before manual heat can start. Older images treat it as off. */
    HEAT_MODE_MANUAL_PENDING = 4,
    HEAT_MODE_LOCKOUT = 5,
} heat_mode_t;

typedef struct {
    /* Keep an unadjusted reading for the safety cutoff. */
    float probe_c;
    /* Calibrated, before filtering. */
    float raw_c;
    float filtered_c;
    bool valid;
    uint8_t consecutive_valid;
    uint8_t consecutive_failures;
    uint64_t timestamp_ms;
    heat_fault_t fault;
} temperature_sample_t;

typedef struct {
    heat_state_t state;
    heat_fault_t fault;
    bool enabled;
    bool restore_pending; /* Legacy telemetry flag. */
    bool heater_on;
    bool fan_on;
    float target_c;
    float restart_c;
    float calibration_offset_c;
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
        .restore_pending = false,
        .heater_on = false,
        .fan_on = false,
        .target_c = 60.0f,
        .restart_c = 59.0f,
        .calibration_offset_c = 0.0f,
        .temperature = { .valid = false },
        .wifi_rssi = 0,
        .ip_addr = "0.0.0.0",
        .uptime_ms = 0,
    };
}
