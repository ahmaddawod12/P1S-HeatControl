#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_types.h"
#include "esp_err.h"
#include "firmware_types.h"
#include "wifi_manager.h"
#include "printer_types.h"

/* Injected service boundary so handler tests never start the real
 * controller. */
typedef struct {
    void (*get_status)(app_status_t *out_status);
    esp_err_t (*set_target)(float target_c);
    esp_err_t (*set_calibration)(float calibration_offset_c);
    esp_err_t (*set_enabled)(bool enabled);
    esp_err_t (*set_manual_fan)(bool enabled);
    esp_err_t (*ack_fault)(void);
    void (*get_firmware_status)(firmware_status_t *out);
    esp_err_t (*request_firmware_update)(void);
    void (*get_wifi_status)(wifi_manager_status_t *out);
    esp_err_t (*request_wifi_change)(const wifi_credentials_t *credentials);
    void (*get_printer_status)(printer_control_status_t *out);
    esp_err_t (*request_printer_config)(const printer_config_request_t *request);
    esp_err_t (*forget_printer_config)(void);
    esp_err_t (*arm_printer_cycle)(const printer_arm_request_t *request);
    esp_err_t (*stop_printer_cycle)(void);
} web_server_ops_t;

typedef struct {
    web_server_ops_t ops;
    uint16_t port;
} web_server_config_t;

/* Production wiring: app_runtime on port 80. */
web_server_config_t web_server_default_config(void);

/* Byte count to serve for an asset embedded with EMBED_TXTFILES, whose
 * `end` symbol sits one past a NUL terminator the linker appended. Serving
 * that NUL makes the browser reject app.js with a SyntaxError. Returns 0 for
 * a null or empty span. */
size_t web_server_asset_length(const char *start, const char *end);

esp_err_t web_server_start(const web_server_config_t *config);
esp_err_t web_server_stop(void);
