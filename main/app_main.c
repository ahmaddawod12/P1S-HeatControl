#include <stddef.h>

#include "app_startup.h"
#include "board_outputs.h"
#include "esp_log.h"
#include "firmware_update.h"
#include "sdkconfig.h"

static const char *TAG = "p1s_heat";

void app_main(void)
{
#ifdef CONFIG_P1S_FIREBASE_ENABLED
    const app_startup_ops_t ops = app_startup_default_ops();

    bool runtime_ready;
    const esp_err_t err = app_startup_run(&ops, &runtime_ready);
    firmware_trial_startup_complete(err);
    if (!runtime_ready) {
        ESP_LOGE(TAG, "controller startup failed: %s", esp_err_to_name(err));
        return;
    }
    const esp_err_t update_err = firmware_update_start();
    if (update_err != ESP_OK) ESP_LOGE(TAG, "updater unavailable: %s", esp_err_to_name(update_err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "controller services started");
        return;
    }

    ESP_LOGW(TAG, "retrying network services after startup error: %s", esp_err_to_name(err));
    app_startup_retry_services(&ops);
    firmware_trial_startup_complete(ESP_OK);
#else
    const esp_err_t err = board_outputs_init(NULL);
    if (err != ESP_OK) {
        (void) board_outputs_force_off();
        ESP_LOGE(TAG, "relay init failed: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "Firebase configuration is required to start the controller");
#endif
}
