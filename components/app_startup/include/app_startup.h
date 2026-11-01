#pragma once

#include "app_types.h"
#include "esp_err.h"

/* Startup is a table of steps so the order and the failure policy can be
 * asserted without bringing real hardware up. */
typedef struct {
    esp_err_t (*board_outputs_init)(void);
    esp_err_t (*trial_start)(void);
    esp_err_t (*nvs_flash_init)(void);
    esp_err_t (*settings_init)(void);
    esp_err_t (*runtime_start)(void);
    esp_err_t (*wifi_start)(void);
    esp_err_t (*web_server_start)(void);
    void (*board_outputs_force_off)(void);
    esp_err_t (*report_startup_fault)(heat_fault_t fault);
} app_startup_ops_t;

app_startup_ops_t app_startup_default_ops(void);

/* Runs the steps in their fixed order. Relay outputs are commanded off on
 * any failure, and nothing here ever enables regulation. Returns the first
 * error encountered; a networking or web-server failure still leaves the
 * runtime regulating so the caller can retry. */
esp_err_t app_startup_run(const app_startup_ops_t *ops);

/* Retries networking and the web server until both are up. */
void app_startup_retry_services(const app_startup_ops_t *ops);
