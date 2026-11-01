#include "temperature_sensor.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

temperature_sensor_config_t temperature_sensor_default_config(gpio_num_t gpio)
{
    return (temperature_sensor_config_t) {.gpio = gpio};
}

esp_err_t temperature_sensor_init(temperature_sensor_t *sensor,
                                  const temperature_sensor_config_t *config)
{
    if (sensor == NULL || config == NULL ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sensor->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(sensor, 0, sizeof(*sensor));

    const onewire_bus_config_t bus_config = {
        .bus_gpio_num = config->gpio,
        .flags.en_pull_up = false,
    };
    const onewire_bus_rmt_config_t rmt_config = {.max_rx_bytes = 10};
    esp_err_t err = onewire_new_bus_rmt(&bus_config, &rmt_config, &sensor->bus);
    if (err != ESP_OK) {
        return err;
    }

    /* No presence check here: an absent probe must not prevent the runtime
     * and dashboard from starting. Each read checks it again. */
    ds18b20_config_t device_config;
    memset(&device_config, 0, sizeof(device_config));
    err = ds18b20_new_device_from_bus(sensor->bus, &device_config, &sensor->device);
    if (err != ESP_OK) {
        (void) onewire_bus_del(sensor->bus);
        memset(sensor, 0, sizeof(*sensor));
        return err;
    }
    sensor->config = *config;
    sensor->initialized = true;
    return ESP_OK;
}

esp_err_t temperature_sensor_read(temperature_sensor_t *sensor,
                                  temperature_sensor_reading_t *reading)
{
    if (reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *reading = (temperature_sensor_reading_t) {
        .temperature_c = NAN,
        .fault = TEMPERATURE_SENSOR_FAULT_IO,
    };
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sensor->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gpio_get_level(sensor->config.gpio) == 0) {
        reading->fault = TEMPERATURE_SENSOR_FAULT_SHORT;
        return ESP_OK;
    }

    /* A hot-plugged probe powers up at 12 bits. Reapply 11 bits every time,
     * then wait for a fresh conversion (400 ms in the vendor driver). */
    esp_err_t err = ds18b20_set_resolution(sensor->device, DS18B20_RESOLUTION_11B);
    if (err == ESP_OK) {
        err = ds18b20_trigger_temperature_conversion(sensor->device);
    }
    float temperature_c = NAN;
    if (err == ESP_OK) {
        /* The vendor validates the scratchpad CRC and power-on sentinel. */
        err = ds18b20_get_temperature(sensor->device, &temperature_c);
    }
    /* A persistent short arising during conversion/read must not turn an
     * incomplete, all-zero (and CRC-valid) scratchpad into a valid 0 C. */
    if (gpio_get_level(sensor->config.gpio) == 0) {
        reading->fault = TEMPERATURE_SENSOR_FAULT_SHORT;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        reading->fault = err == ESP_ERR_NOT_FOUND
            ? TEMPERATURE_SENSOR_FAULT_OPEN : TEMPERATURE_SENSOR_FAULT_IO;
        return err;
    }
    if (!isfinite(temperature_c) || temperature_c < -55.0f || temperature_c > 125.0f) {
        reading->fault = TEMPERATURE_SENSOR_FAULT_RANGE;
        return ESP_OK;
    }

    reading->temperature_c = temperature_c;
    reading->fault = TEMPERATURE_SENSOR_FAULT_NONE;
    return ESP_OK;
}

esp_err_t temperature_sensor_deinit(temperature_sensor_t *sensor)
{
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sensor->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t device_err = ds18b20_del_device(sensor->device);
    const esp_err_t bus_err = onewire_bus_del(sensor->bus);
    memset(sensor, 0, sizeof(*sensor));
    return device_err == ESP_OK ? bus_err : device_err;
}
