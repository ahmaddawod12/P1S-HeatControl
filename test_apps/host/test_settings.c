#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "settings.h"

static bool namespace_exists;
static unsigned char blob[sizeof(settings_data_t)];
static size_t blob_size;
static esp_err_t write_error, commit_error;

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { namespace_exists = false; blob_size = 0; return ESP_OK; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    assert(strcmp(name, "heat_cfg") == 0);
    if (!namespace_exists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    namespace_exists = true;
    *out = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    assert(handle == 1 && strcmp(key, "settings") == 0);
    if (!blob_size) return ESP_ERR_NVS_NOT_FOUND;
    if (out && *size < blob_size) return ESP_ERR_NVS_INVALID_LENGTH;
    if (out) memcpy(out, blob, blob_size);
    *size = blob_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1 && strcmp(key, "settings") == 0 && size <= sizeof(blob));
    if (write_error) return write_error;
    memcpy(blob, data, size); blob_size = size;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1); return commit_error; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }

static void seed(uint32_t schema, float target, float offset)
{
    const settings_data_t data = {schema, target, offset};
    namespace_exists = true;
    blob_size = sizeof(data);
    memcpy(blob, &data, blob_size);
    write_error = commit_error = ESP_OK;
}

static void test_ntc_offset_is_reset_once_without_losing_target(void)
{
    seed(1, 55.5f, -7.0f);
    settings_data_t data;
    settings_load_result_t result;
    assert(settings_load(&data, &result) == ESP_OK);
    assert(result == SETTINGS_LOAD_LOADED);
    assert(data.target_c == 55.5f);
    assert(data.calibration_offset_c == 0.0f);
    assert(data.schema_version == 2);
    /* The migrated record is persisted, rather than reset on every read. */
    assert(settings_set_calibration(-0.5f) == ESP_OK);
    assert(settings_load(&data, &result) == ESP_OK);
    assert(data.target_c == 55.5f && data.calibration_offset_c == -0.5f);
    assert(data.schema_version == 2);
}

static void test_migration_failures_are_not_reported_as_success(void)
{
    settings_data_t data;
    settings_load_result_t result;
    seed(1, 80.0f, 10.0f); write_error = ESP_FAIL;
    assert(settings_load(&data, &result) == ESP_FAIL);
    seed(1, 80.0f, 10.0f); commit_error = ESP_FAIL;
    assert(settings_load(&data, &result) == ESP_FAIL);
}

static void test_corrupt_legacy_and_future_records_recover_defaults(void)
{
    const settings_data_t invalid[] = {
        {1, 81.0f, 0.0f}, {1, 60.0f, NAN}, {99, 55.5f, 0.0f},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        seed(invalid[i].schema_version, invalid[i].target_c, invalid[i].calibration_offset_c);
        settings_data_t data;
        settings_load_result_t result;
        assert(settings_load(&data, &result) == ESP_OK);
        assert(result == SETTINGS_LOAD_RECOVERED_CORRUPT);
        assert(data.schema_version == 2 && data.target_c == 60.0f && data.calibration_offset_c == 0.0f);
    }
}

int main(void)
{
    test_ntc_offset_is_reset_once_without_losing_target();
    test_migration_failures_are_not_reported_as_success();
    test_corrupt_legacy_and_future_records_recover_defaults();
    puts("settings: PASS");
    return 0;
}
