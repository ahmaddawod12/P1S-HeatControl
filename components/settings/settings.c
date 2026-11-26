#include "settings.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "heat_controller.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SETTINGS_NAMESPACE "heat_cfg"
#define SETTINGS_KEY "settings"
#define HEAT_MODE_KEY "heat_enabled"
/* A separate key keeps the target and calibration record readable after rollback. */
#define HYSTERESIS_KEY "hysteresis"

_Static_assert(sizeof(settings_data_t) == 12,
               "settings_data_t layout is part of the NVS schema");

static bool calibration_is_valid(float value)
{
    return isfinite(value) && value >= SETTINGS_MIN_CALIBRATION_C &&
           value <= SETTINGS_MAX_CALIBRATION_C;
}

static bool data_is_valid(const settings_data_t *data)
{
    return data != NULL && data->schema_version == SETTINGS_SCHEMA_VERSION &&
           heat_target_is_valid(data->target_c) &&
           calibration_is_valid(data->calibration_offset_c);
}

static esp_err_t store(nvs_handle_t handle, const settings_data_t *data)
{
    esp_err_t err = nvs_set_blob(handle, SETTINGS_KEY, data, sizeof(*data));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    return err;
}

static esp_err_t recover_defaults(nvs_handle_t handle,
                                  settings_data_t *data,
                                  settings_load_result_t result,
                                  settings_load_result_t *out_result)
{
    settings_factory_defaults(data);
    const esp_err_t err = store(handle, data);
    if (err == ESP_OK) {
        *out_result = result;
    }
    return err;
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

void settings_factory_defaults(settings_data_t *data)
{
    if (data == NULL) {
        return;
    }
    *data = (settings_data_t) {
        .schema_version = SETTINGS_SCHEMA_VERSION,
        .target_c = SETTINGS_DEFAULT_TARGET_C,
        .calibration_offset_c = SETTINGS_DEFAULT_CALIBRATION_C,
    };
}

esp_err_t settings_load(settings_data_t *data,
                        settings_load_result_t *out_result)
{
    if (data == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool namespace_existed = false;
    nvs_handle_t probe;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &probe);
    if (err == ESP_OK) {
        namespace_existed = true;
        nvs_close(probe);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = 0;
    err = nvs_get_blob(handle, SETTINGS_KEY, NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND && !namespace_existed) {
        err = recover_defaults(handle, data,
                               SETTINGS_LOAD_DEFAULT_CREATED, out_result);
        nvs_close(handle);
        return err;
    }
    if (err != ESP_OK || size != sizeof(*data)) {
        err = recover_defaults(handle, data,
                               SETTINGS_LOAD_RECOVERED_CORRUPT, out_result);
        nvs_close(handle);
        return err;
    }

    err = nvs_get_blob(handle, SETTINGS_KEY, data, &size);
    if (err == ESP_OK && data->schema_version == 1U &&
        heat_target_is_valid(data->target_c) &&
        calibration_is_valid(data->calibration_offset_c)) {
        /* Preserve the selected target, but do not calibrate the new digital
         * sensor with an offset measured for the old analog divider. */
        data->schema_version = SETTINGS_SCHEMA_VERSION;
        data->calibration_offset_c = SETTINGS_DEFAULT_CALIBRATION_C;
        err = store(handle, data);
        if (err == ESP_OK) {
            *out_result = SETTINGS_LOAD_LOADED;
        }
        nvs_close(handle);
        return err;
    }
    if (err != ESP_OK || !data_is_valid(data)) {
        err = recover_defaults(handle, data,
                               SETTINGS_LOAD_RECOVERED_CORRUPT, out_result);
        nvs_close(handle);
        return err;
    }

    *out_result = SETTINGS_LOAD_LOADED;
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t update(float value, bool target)
{
    if ((target && !heat_target_is_valid(value)) ||
        (!target && !calibration_is_valid(value))) {
        return ESP_ERR_INVALID_ARG;
    }

    settings_data_t data;
    settings_load_result_t result;
    esp_err_t err = settings_load(&data, &result);
    if (err != ESP_OK) {
        return err;
    }

    if (target) {
        data.target_c = value;
    } else {
        data.calibration_offset_c = value;
    }

    nvs_handle_t handle;
    err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = store(handle, &data);
        nvs_close(handle);
    }
    return err;
}

esp_err_t settings_set_target(float target_c)
{
    return update(target_c, true);
}

esp_err_t settings_set_calibration(float calibration_offset_c)
{
    return update(calibration_offset_c, false);
}

esp_err_t settings_load_hysteresis(float *hysteresis_c)
{
    if (hysteresis_c == NULL) return ESP_ERR_INVALID_ARG;
    *hysteresis_c = HEAT_DEFAULT_HYSTERESIS_C;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;

    uint8_t half_degrees;
    err = nvs_get_u8(handle, HYSTERESIS_KEY, &half_degrees);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    const float value = half_degrees * 0.5f;
    if (!heat_hysteresis_is_valid(value)) return ESP_ERR_INVALID_STATE;
    *hysteresis_c = value;
    return ESP_OK;
}

esp_err_t settings_set_hysteresis(float hysteresis_c)
{
    if (!heat_hysteresis_is_valid(hysteresis_c)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, HYSTERESIS_KEY, (uint8_t) (hysteresis_c * 2.0f));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

esp_err_t settings_load_heat_mode(heat_mode_t *mode)
{
    if (mode == NULL) return ESP_ERR_INVALID_ARG;
    *mode = HEAT_MODE_OFF;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;

    uint8_t value = 0;
    err = nvs_get_u8(handle, HEAT_MODE_KEY, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (value > HEAT_MODE_LOCKOUT) return ESP_ERR_INVALID_STATE;
    *mode = (heat_mode_t) value;
    return ESP_OK;
}

esp_err_t settings_save_heat_mode(heat_mode_t mode)
{
    if ((unsigned) mode > HEAT_MODE_LOCKOUT) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, HEAT_MODE_KEY, (uint8_t) mode);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}
