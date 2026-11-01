#include "app_startup.h"

#include <stddef.h>

#include "app_runtime.h"
#include "board_outputs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "settings.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "firmware_update.h"

#define RETRY_PERIOD_MS 10000U

static const char *TAG = "startup";

static esp_err_t default_board_init(void)
{
    return board_outputs_init(NULL);
}

/* Erases once and retries, which is the documented recovery for a version
 * bump or a full partition. */
static esp_err_t default_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

static esp_err_t default_web_start(void)
{
    const web_server_config_t config = web_server_default_config();
    return web_server_start(&config);
}

static void default_force_off(void)
{
    (void) board_outputs_force_off();
}

app_startup_ops_t app_startup_default_ops(void)
{
    return (app_startup_ops_t) {
        .board_outputs_init = default_board_init,
        .trial_start = firmware_trial_start,
        .nvs_flash_init = default_nvs_init,
        .settings_init = settings_init,
        .runtime_start = app_runtime_start,
        .wifi_start = wifi_manager_start,
        .web_server_start = default_web_start,
        .board_outputs_force_off = default_force_off,
        .report_startup_fault = app_runtime_report_startup_fault,
    };
}

static bool ops_are_complete(const app_startup_ops_t *ops)
{
    return ops != NULL && ops->board_outputs_init != NULL &&
           ops->nvs_flash_init != NULL && ops->settings_init != NULL &&
           ops->runtime_start != NULL && ops->wifi_start != NULL &&
           ops->web_server_start != NULL &&
           ops->board_outputs_force_off != NULL &&
           ops->report_startup_fault != NULL;
}

esp_err_t app_startup_run(const app_startup_ops_t *ops)
{
    if (!ops_are_complete(ops)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Relays first: nothing else may run while the outputs are unknown. */
    esp_err_t err = ops->board_outputs_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "relay init failed (%s); latching %d",
                 esp_err_to_name(err), (int) HEAT_FAULT_OUTPUT_INIT);
        ops->board_outputs_force_off();
        return err;
    }

    if (ops->trial_start != NULL) {
        err = ops->trial_start();
        if (err != ESP_OK) { ops->board_outputs_force_off(); return err; }
    }
    err = ops->nvs_flash_init();
    firmware_trial_startup_progress();
    if (err != ESP_OK) {
        /* Settings fall back to validated defaults, so keep going. */
        ESP_LOGW(TAG, "nvs init failed: %s", esp_err_to_name(err));
    }

    const esp_err_t settings_err = ops->settings_init();
    firmware_trial_startup_progress();
    if (settings_err != ESP_OK) {
        ESP_LOGW(TAG, "settings init failed: %s", esp_err_to_name(settings_err));
    }

    err = ops->runtime_start();
    firmware_trial_startup_progress();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime start failed: %s", esp_err_to_name(err));
        ops->board_outputs_force_off();
        return err;
    }

    /* From here the runtime regulates on its own; service failures are
     * reported and retried rather than fatal. */
    esp_err_t first_error = ESP_OK;

    err = ops->wifi_start();
    firmware_trial_startup_progress();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        ops->board_outputs_force_off();
        first_error = err;
    }

    err = ops->web_server_start();
    firmware_trial_startup_progress();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server start failed: %s", esp_err_to_name(err));
        ops->board_outputs_force_off();
        if (first_error == ESP_OK) {
            first_error = err;
        }
    }

    return first_error;
}

void app_startup_retry_services(const app_startup_ops_t *ops)
{
    if (!ops_are_complete(ops)) {
        return;
    }

    bool wifi_up = false;
    bool web_up = false;
    while (!wifi_up || !web_up) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_PERIOD_MS));

        if (!wifi_up) {
            const esp_err_t err = ops->wifi_start();
            wifi_up = err == ESP_OK;
            if (!wifi_up) {
                ESP_LOGW(TAG, "wifi retry failed: %s", esp_err_to_name(err));
            }
        }
        if (!web_up) {
            const esp_err_t err = ops->web_server_start();
            web_up = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
            if (!web_up) {
                ESP_LOGW(TAG, "web retry failed: %s", esp_err_to_name(err));
            }
        }
    }
    ESP_LOGI(TAG, "networking and web server are up");
}
