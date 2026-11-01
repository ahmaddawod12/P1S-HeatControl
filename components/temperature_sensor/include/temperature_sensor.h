#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "ds18b20.h"
#include "esp_err.h"
#include "onewire_bus.h"
#include "temperature_sensor_types.h"

typedef struct {
    gpio_num_t gpio;
} temperature_sensor_config_t;

/* One externally powered DS18B20 per bus, with an external pull-up to 3.3 V.
 * Zero-initialize before first use; all operations belong to one task. */
typedef struct {
    temperature_sensor_config_t config;
    onewire_bus_handle_t bus;
    ds18b20_device_handle_t device;
    bool initialized;
} temperature_sensor_t;

temperature_sensor_config_t temperature_sensor_default_config(gpio_num_t gpio);
esp_err_t temperature_sensor_init(temperature_sensor_t *sensor,
                                  const temperature_sensor_config_t *config);
/* Always initializes a non-null reading, even on failure. Hardware errors
 * return their esp_err_t with a diagnostic fault; range/short faults return
 * ESP_OK with an invalid reading. Never use temperature_c unless fault is NONE. */
esp_err_t temperature_sensor_read(temperature_sensor_t *sensor,
                                  temperature_sensor_reading_t *reading);
esp_err_t temperature_sensor_deinit(temperature_sensor_t *sensor);
