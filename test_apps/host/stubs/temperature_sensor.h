#pragma once
#include <stdbool.h>
#include "temperature_sensor_types.h"
#include "esp_err.h"

typedef struct { bool initialized; } temperature_sensor_t;
typedef struct { int gpio; } temperature_sensor_config_t;
temperature_sensor_config_t temperature_sensor_default_config(int gpio);
esp_err_t temperature_sensor_init(temperature_sensor_t *, const temperature_sensor_config_t *);
esp_err_t temperature_sensor_read(temperature_sensor_t *, temperature_sensor_reading_t *);
esp_err_t temperature_sensor_deinit(temperature_sensor_t *);
