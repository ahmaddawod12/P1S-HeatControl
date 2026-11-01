#pragma once

#include "onewire_bus.h"

typedef struct fake_ds18b20 *ds18b20_device_handle_t;
typedef struct { int unused; } ds18b20_config_t;
typedef enum {
    DS18B20_RESOLUTION_9B,
    DS18B20_RESOLUTION_10B,
    DS18B20_RESOLUTION_11B,
    DS18B20_RESOLUTION_12B,
} ds18b20_resolution_t;

esp_err_t ds18b20_new_device_from_bus(onewire_bus_handle_t,
                                      const ds18b20_config_t *,
                                      ds18b20_device_handle_t *);
esp_err_t ds18b20_del_device(ds18b20_device_handle_t);
esp_err_t ds18b20_set_resolution(ds18b20_device_handle_t, ds18b20_resolution_t);
esp_err_t ds18b20_trigger_temperature_conversion(ds18b20_device_handle_t);
esp_err_t ds18b20_get_temperature(ds18b20_device_handle_t, float *);
