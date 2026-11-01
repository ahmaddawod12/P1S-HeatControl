#include "app_startup.h"
#include "esp_log.h"
#include "firmware_update.h"

static const char *TAG = "p1s_heat";

void app_main(void)
{
    const app_startup_ops_t ops = app_startup_default_ops();

    const esp_err_t err = app_startup_run(&ops);
    firmware_trial_startup_complete(err);
    const esp_err_t update_err = firmware_update_start();
    if (update_err != ESP_OK) ESP_LOGE(TAG, "updater unavailable: %s", esp_err_to_name(update_err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "controller ready and disabled; open http://p1s-heat.local");
        return;
    }

    /* Relay or runtime failures leave nothing to retry; the outputs are
     * already off and the board needs attention. */
    ESP_LOGE(TAG, "startup incomplete: %s", esp_err_to_name(err));
    app_startup_retry_services(&ops);
}
