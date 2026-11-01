/* Exercise the real adapter; replace only the unavailable GPIO/RMT hardware
 * and the vendor's on-wire operations. */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "temperature_sensor.h"

static int bus_storage, sensor_storage;
static esp_err_t bus_error, create_error, resolution_error, conversion_error, read_error;
static int level = 1, level_after_read = 1, live_buses, live_sensors, phase;
static float wire_temperature = 25.125f;
static ds18b20_resolution_t wire_resolution;

int gpio_get_level(gpio_num_t gpio) { assert(gpio == 27); return level; }
esp_err_t onewire_new_bus_rmt(const onewire_bus_config_t *config,
                             const onewire_bus_rmt_config_t *rmt,
                             onewire_bus_handle_t *out)
{
    assert(config->bus_gpio_num == 27 && rmt->max_rx_bytes >= 9);
    if (bus_error) return bus_error;
    *out = (onewire_bus_handle_t) &bus_storage;
    ++live_buses;
    return ESP_OK;
}
esp_err_t onewire_bus_del(onewire_bus_handle_t bus)
{ assert(bus == (onewire_bus_handle_t) &bus_storage); --live_buses; return ESP_OK; }
esp_err_t ds18b20_new_device_from_bus(onewire_bus_handle_t bus,
                                      const ds18b20_config_t *config,
                                      ds18b20_device_handle_t *out)
{
    assert(bus && config);
    if (create_error) return create_error;
    *out = (ds18b20_device_handle_t) &sensor_storage;
    ++live_sensors;
    return ESP_OK;
}
esp_err_t ds18b20_del_device(ds18b20_device_handle_t sensor)
{ assert(sensor == (ds18b20_device_handle_t) &sensor_storage); --live_sensors; return ESP_OK; }
esp_err_t ds18b20_set_resolution(ds18b20_device_handle_t sensor,
                                ds18b20_resolution_t resolution)
{
    assert(sensor && phase == 0);
    if (resolution_error) return resolution_error;
    wire_resolution = resolution;
    phase = 1;
    return ESP_OK;
}
esp_err_t ds18b20_trigger_temperature_conversion(ds18b20_device_handle_t sensor)
{
    assert(sensor && phase == 1);
    /* A fresh 11-bit conversion must precede every published temperature,
     * including after a probe power cycle resets its resolution to 12-bit. */
    assert(wire_resolution == DS18B20_RESOLUTION_11B);
    phase = 2;
    return conversion_error;
}
esp_err_t ds18b20_get_temperature(ds18b20_device_handle_t sensor, float *out)
{
    assert(sensor && phase == 2);
    phase = 3;
    if (read_error) return read_error;
    *out = wire_temperature;
    level = level_after_read;
    return ESP_OK;
}

static void reset_wire(void)
{
    bus_error = create_error = resolution_error = conversion_error = read_error = ESP_OK;
    phase = 0;
    level = 1;
    level_after_read = 1;
    wire_temperature = 25.125f;
    wire_resolution = DS18B20_RESOLUTION_12B;
}

static void test_initialization_and_cleanup(void)
{
    temperature_sensor_t sensor = {0};
    temperature_sensor_config_t config = temperature_sensor_default_config(34);
    assert(temperature_sensor_init(&sensor, &config) == ESP_ERR_INVALID_ARG);
    config = temperature_sensor_default_config(27);
    assert(temperature_sensor_init(NULL, &config) == ESP_ERR_INVALID_ARG);
    assert(temperature_sensor_init(&sensor, NULL) == ESP_ERR_INVALID_ARG);

    bus_error = ESP_ERR_NO_MEM;
    assert(temperature_sensor_init(&sensor, &config) == ESP_ERR_NO_MEM);
    assert(!sensor.initialized && live_buses == 0 && live_sensors == 0);
    bus_error = ESP_OK;
    create_error = ESP_ERR_NO_MEM;
    assert(temperature_sensor_init(&sensor, &config) == ESP_ERR_NO_MEM);
    assert(!sensor.initialized && live_buses == 0 && live_sensors == 0);
    reset_wire();

    /* Missing presence must be a reading fault, not prevent startup. */
    resolution_error = ESP_ERR_NOT_FOUND;
    assert(temperature_sensor_init(&sensor, &config) == ESP_OK);
    assert(sensor.initialized);
    assert(temperature_sensor_init(&sensor, &config) == ESP_ERR_INVALID_STATE);
    assert(live_buses == 1 && live_sensors == 1);
    temperature_sensor_reading_t reading;
    assert(temperature_sensor_read(&sensor, &reading) == ESP_ERR_NOT_FOUND);
    assert(reading.fault == TEMPERATURE_SENSOR_FAULT_OPEN && isnan(reading.temperature_c));
    assert(temperature_sensor_deinit(&sensor) == ESP_OK);
    assert(!sensor.initialized && !sensor.bus && !sensor.device);
    assert(live_buses == 0 && live_sensors == 0);
    assert(temperature_sensor_deinit(&sensor) == ESP_ERR_INVALID_STATE);
    assert(temperature_sensor_read(&sensor, &reading) == ESP_ERR_INVALID_STATE);
    assert(isnan(reading.temperature_c));
}

static void test_fresh_conversions_and_fault_recovery(void)
{
    reset_wire();
    temperature_sensor_t sensor = {0};
    const temperature_sensor_config_t config = temperature_sensor_default_config(27);
    assert(temperature_sensor_init(&sensor, &config) == ESP_OK);
    temperature_sensor_reading_t reading;
    assert(temperature_sensor_read(&sensor, &reading) == ESP_OK);
    assert(phase == 3 && reading.fault == TEMPERATURE_SENSOR_FAULT_NONE);
    assert(reading.temperature_c == 25.125f);

    reset_wire(); level = 0;
    assert(temperature_sensor_read(&sensor, &reading) == ESP_OK);
    assert(phase == 0 && reading.fault == TEMPERATURE_SENSOR_FAULT_SHORT);
    assert(isnan(reading.temperature_c));

    reset_wire(); resolution_error = ESP_ERR_NOT_FOUND;
    assert(temperature_sensor_read(&sensor, &reading) == ESP_ERR_NOT_FOUND);
    assert(phase == 0 && reading.fault == TEMPERATURE_SENSOR_FAULT_OPEN);
    assert(isnan(reading.temperature_c));

    reset_wire(); conversion_error = ESP_ERR_TIMEOUT;
    assert(temperature_sensor_read(&sensor, &reading) == ESP_ERR_TIMEOUT);
    assert(phase == 2 && reading.fault == TEMPERATURE_SENSOR_FAULT_IO);
    assert(isnan(reading.temperature_c));

    reset_wire(); read_error = ESP_ERR_INVALID_CRC;
    assert(temperature_sensor_read(&sensor, &reading) == ESP_ERR_INVALID_CRC);
    assert(reading.fault == TEMPERATURE_SENSOR_FAULT_IO && isnan(reading.temperature_c));

    /* A short beginning during acquisition can appear as an all-zero,
     * CRC-valid scratchpad to the vendor driver. It must not publish 0 C. */
    reset_wire(); wire_temperature = 0.0f; level_after_read = 0;
    assert(temperature_sensor_read(&sensor, &reading) == ESP_OK);
    assert(reading.fault == TEMPERATURE_SENSOR_FAULT_SHORT && isnan(reading.temperature_c));

    reset_wire(); read_error = ESP_ERR_INVALID_STATE; /* Vendor power-on sentinel. */
    assert(temperature_sensor_read(&sensor, &reading) == ESP_ERR_INVALID_STATE);
    assert(reading.fault == TEMPERATURE_SENSOR_FAULT_IO && isnan(reading.temperature_c));

    /* Reconnection must reconfigure a sensor that has reverted to 12 bits. */
    reset_wire();
    assert(temperature_sensor_read(&sensor, &reading) == ESP_OK);
    assert(reading.fault == TEMPERATURE_SENSOR_FAULT_NONE && reading.temperature_c == 25.125f);
    assert(temperature_sensor_deinit(&sensor) == ESP_OK);
}

static void test_range_and_nonfinite_values(void)
{
    reset_wire();
    temperature_sensor_t sensor = {0};
    const temperature_sensor_config_t config = temperature_sensor_default_config(27);
    assert(temperature_sensor_init(&sensor, &config) == ESP_OK);
    const float valid[] = {-55.0f, -10.125f, 0.0f, 82.0f, 85.0f, 125.0f};
    temperature_sensor_reading_t reading;
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        reset_wire(); wire_temperature = valid[i];
        assert(temperature_sensor_read(&sensor, &reading) == ESP_OK);
        assert(reading.fault == TEMPERATURE_SENSOR_FAULT_NONE);
        assert(reading.temperature_c == valid[i]);
    }
    const float invalid[] = {-127.0f, -55.125f, 125.125f, NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset_wire(); wire_temperature = invalid[i];
        assert(temperature_sensor_read(&sensor, &reading) == ESP_OK);
        assert(reading.fault == TEMPERATURE_SENSOR_FAULT_RANGE);
        assert(isnan(reading.temperature_c));
    }
    assert(temperature_sensor_deinit(&sensor) == ESP_OK);
}

int main(void)
{
    test_initialization_and_cleanup();
    test_fresh_conversions_and_fault_recovery();
    test_range_and_nonfinite_values();
    puts("temperature_sensor: PASS");
    return 0;
}
