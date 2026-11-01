#include <math.h>

#include "board_pins.h"
#include "temperature_sensor.h"
#include "unity.h"

TEST_CASE("DS18B20 bus initializes without a probe and releases RMT resources",
          "[temperature_sensor]")
{
    const temperature_sensor_config_t config =
        temperature_sensor_default_config(BOARD_TEMPERATURE_SENSOR_GPIO);
    TEST_ASSERT_EQUAL(GPIO_NUM_27, config.gpio);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        temperature_sensor_t sensor = {0};
        TEST_ESP_OK(temperature_sensor_init(&sensor, &config));
        TEST_ASSERT_TRUE(sensor.initialized);
        TEST_ASSERT_NOT_NULL(sensor.bus);
        TEST_ASSERT_NOT_NULL(sensor.device);
        TEST_ESP_OK(temperature_sensor_deinit(&sensor));
        TEST_ASSERT_FALSE(sensor.initialized);
        TEST_ASSERT_NULL(sensor.bus);
        TEST_ASSERT_NULL(sensor.device);
    }
}

TEST_CASE("DS18B20 rejects input-only GPIO and uninitialized reads",
          "[temperature_sensor]")
{
    temperature_sensor_t sensor = {0};
    temperature_sensor_reading_t reading;
    const temperature_sensor_config_t input_only =
        temperature_sensor_default_config(GPIO_NUM_34);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, temperature_sensor_init(&sensor, &input_only));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, temperature_sensor_read(&sensor, &reading));
    TEST_ASSERT_TRUE(isnan(reading.temperature_c));
    TEST_ASSERT_EQUAL(TEMPERATURE_SENSOR_FAULT_IO, reading.fault);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, temperature_sensor_read(NULL, &reading));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, temperature_sensor_read(&sensor, NULL));
}
