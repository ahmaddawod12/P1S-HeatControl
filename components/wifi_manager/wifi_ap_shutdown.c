#include "wifi_ap_shutdown.h"
#include "service_operation.h"
esp_err_t wifi_ap_shutdown(const wifi_ap_shutdown_ops_t *ops)
{
    if (!service_operation_begin(SERVICE_OPERATION_WIFI)) return ESP_ERR_NOT_ALLOWED;
    const uint32_t before=ops->disconnect_epoch(ops->ctx);
    esp_err_t err=ESP_ERR_NOT_ALLOWED;
    if (!ops->disconnect_pending(ops->ctx)) {
        err=ops->set_ap(false,ops->ctx);
        if (err==ESP_OK && (ops->disconnect_epoch(ops->ctx)!=before ||
            ops->disconnect_pending(ops->ctx))) {
            err=ops->set_ap(true,ops->ctx);
            if (err==ESP_OK) err=ESP_ERR_NOT_ALLOWED;
        }
    }
    service_operation_end(SERVICE_OPERATION_WIFI);
    return err;
}
