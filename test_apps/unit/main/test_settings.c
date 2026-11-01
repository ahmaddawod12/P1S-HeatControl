#include <stddef.h>
#include <stdint.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "settings.h"
#include "unity.h"

static void reset_nvs(void)
{
    nvs_flash_deinit();
    TEST_ESP_OK(nvs_flash_erase());
    TEST_ESP_OK(nvs_flash_init());
}

TEST_CASE("empty NVS loads versioned defaults", "[settings]")
{
    reset_nvs();

    settings_data_t data;
    settings_load_result_t result = SETTINGS_LOAD_LOADED;
    TEST_ESP_OK(settings_load(&data, &result));

    TEST_ASSERT_EQUAL(SETTINGS_LOAD_DEFAULT_CREATED, result);
    TEST_ASSERT_EQUAL_UINT32(2, data.schema_version);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, data.target_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, data.calibration_offset_c);
}

TEST_CASE("target and calibration persist without enabled state", "[settings]")
{
    reset_nvs();
    TEST_ESP_OK(settings_init());
    TEST_ESP_OK(settings_set_target(55.5f));
    TEST_ESP_OK(settings_set_calibration(-1.25f));

    settings_data_t data;
    settings_load_result_t result = SETTINGS_LOAD_RECOVERED_CORRUPT;
    TEST_ESP_OK(settings_load(&data, &result));

    TEST_ASSERT_EQUAL(SETTINGS_LOAD_LOADED, result);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 55.5f, data.target_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.25f, data.calibration_offset_c);
    TEST_ASSERT_EQUAL(sizeof(settings_data_t),
                      sizeof(data.schema_version) + sizeof(data.target_c) +
                          sizeof(data.calibration_offset_c));
}

TEST_CASE("malformed settings blob is replaced with defaults", "[settings]")
{
    reset_nvs();

    nvs_handle_t handle;
    TEST_ESP_OK(nvs_open("heat_cfg", NVS_READWRITE, &handle));
    const uint8_t malformed[] = {0xde, 0xad, 0xbe, 0xef};
    TEST_ESP_OK(nvs_set_blob(handle, "settings", malformed, sizeof(malformed)));
    TEST_ESP_OK(nvs_commit(handle));
    nvs_close(handle);

    settings_data_t data;
    settings_load_result_t result = SETTINGS_LOAD_LOADED;
    TEST_ESP_OK(settings_load(&data, &result));

    TEST_ASSERT_EQUAL(SETTINGS_LOAD_RECOVERED_CORRUPT, result);
    TEST_ASSERT_EQUAL_UINT32(2, data.schema_version);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, data.target_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, data.calibration_offset_c);

    TEST_ESP_OK(settings_load(&data, &result));
    TEST_ASSERT_EQUAL(SETTINGS_LOAD_LOADED, result);
}

TEST_CASE("invalid settings updates leave stored values unchanged", "[settings]")
{
    reset_nvs();
    TEST_ESP_OK(settings_init());
    TEST_ESP_OK(settings_set_target(55.5f));
    TEST_ESP_OK(settings_set_calibration(2.0f));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_set_target(60.2f));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_set_calibration(11.0f));

    settings_data_t data;
    settings_load_result_t result;
    TEST_ESP_OK(settings_load(&data, &result));
    TEST_ASSERT_EQUAL(SETTINGS_LOAD_LOADED, result);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 55.5f, data.target_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, data.calibration_offset_c);
}

TEST_CASE("unknown settings schema is recovered", "[settings]")
{
    reset_nvs();

    settings_data_t future = {
        .schema_version = 99,
        .target_c = 60.0f,
        .calibration_offset_c = 0.0f,
    };
    nvs_handle_t handle;
    TEST_ESP_OK(nvs_open("heat_cfg", NVS_READWRITE, &handle));
    TEST_ESP_OK(nvs_set_blob(handle, "settings", &future, sizeof(future)));
    TEST_ESP_OK(nvs_commit(handle));
    nvs_close(handle);

    settings_data_t data;
    settings_load_result_t result;
    TEST_ESP_OK(settings_load(&data, &result));
    TEST_ASSERT_EQUAL(SETTINGS_LOAD_RECOVERED_CORRUPT, result);
    TEST_ASSERT_EQUAL_UINT32(2, data.schema_version);
}

TEST_CASE("NTC calibration is reset once while target survives DS18B20 migration", "[settings]")
{
    reset_nvs();
    const settings_data_t legacy = {
        .schema_version = 1,
        .target_c = 55.5f,
        .calibration_offset_c = -7.0f,
    };
    nvs_handle_t handle;
    TEST_ESP_OK(nvs_open("heat_cfg", NVS_READWRITE, &handle));
    TEST_ESP_OK(nvs_set_blob(handle, "settings", &legacy, sizeof(legacy)));
    TEST_ESP_OK(nvs_commit(handle));
    nvs_close(handle);

    settings_data_t data;
    settings_load_result_t result;
    TEST_ESP_OK(settings_load(&data, &result));
    TEST_ASSERT_EQUAL(SETTINGS_LOAD_LOADED, result);
    TEST_ASSERT_EQUAL_UINT32(2, data.schema_version);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 55.5f, data.target_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, data.calibration_offset_c);
    TEST_ESP_OK(settings_set_calibration(-0.5f));
    TEST_ESP_OK(settings_load(&data, &result));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.5f, data.calibration_offset_c);
}
