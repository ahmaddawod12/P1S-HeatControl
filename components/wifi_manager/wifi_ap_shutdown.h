#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    esp_err_t (*set_ap)(bool enabled,void *ctx);
    uint32_t (*disconnect_epoch)(void *ctx);
    bool (*disconnect_pending)(void *ctx);
    void *ctx;
} wifi_ap_shutdown_ops_t;

/* ESP_ERR_NOT_ALLOWED means deferred with AP retained, including a late
 * disconnect successfully recovered across the driver call. */
esp_err_t wifi_ap_shutdown(const wifi_ap_shutdown_ops_t *ops);
