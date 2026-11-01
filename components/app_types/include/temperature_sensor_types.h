#pragma once

typedef enum {
    TEMPERATURE_SENSOR_FAULT_NONE = 0,
    TEMPERATURE_SENSOR_FAULT_OPEN,
    TEMPERATURE_SENSOR_FAULT_SHORT,
    TEMPERATURE_SENSOR_FAULT_RANGE,
    TEMPERATURE_SENSOR_FAULT_IO,
} temperature_sensor_fault_t;

typedef struct {
    float temperature_c;
    temperature_sensor_fault_t fault;
} temperature_sensor_reading_t;
