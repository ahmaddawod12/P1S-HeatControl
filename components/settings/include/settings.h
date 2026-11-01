#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Schema 2 calibrates the DS18B20; schema 1 offsets belong to the NTC. */
#define SETTINGS_SCHEMA_VERSION 2U
#define SETTINGS_DEFAULT_TARGET_C 60.0f
#define SETTINGS_DEFAULT_CALIBRATION_C 0.0f
#define SETTINGS_MIN_CALIBRATION_C (-10.0f)
#define SETTINGS_MAX_CALIBRATION_C 10.0f

typedef enum {
    SETTINGS_LOAD_LOADED = 0,
    SETTINGS_LOAD_DEFAULT_CREATED,
    SETTINGS_LOAD_RECOVERED_CORRUPT,
} settings_load_result_t;

typedef struct {
    uint32_t schema_version;
    float target_c;
    float calibration_offset_c;
} settings_data_t;

esp_err_t settings_init(void);
void settings_factory_defaults(settings_data_t *data);
esp_err_t settings_load(settings_data_t *data,
                        settings_load_result_t *out_result);
esp_err_t settings_set_target(float target_c);
esp_err_t settings_set_calibration(float calibration_offset_c);
