#include "wifi_ap_shutdown.h"
#include "service_operation.h"
#include <assert.h>
#include <stdio.h>

typedef struct {
    bool active,pending,disconnect_during_stop,save_during_stop,admitted;
    unsigned calls;
    uint32_t epoch;
    esp_err_t stop_error,restore_error;
} fixture_t;
static esp_err_t set_ap(bool enabled,void *ctx)
{
    fixture_t *f=ctx; ++f->calls;
    if (!enabled && f->save_during_stop) f->admitted=service_operation_begin(SERVICE_OPERATION_WIFI);
    if (!enabled && f->disconnect_during_stop) { ++f->epoch; f->pending=true; }
    const esp_err_t err=enabled?f->restore_error:f->stop_error;
    if (err==ESP_OK) f->active=enabled;
    return err;
}
static uint32_t epoch(void *ctx) { return ((fixture_t *)ctx)->epoch; }
static bool pending(void *ctx) { return ((fixture_t *)ctx)->pending; }
static esp_err_t run(fixture_t *f)
{
    const wifi_ap_shutdown_ops_t ops={.set_ap=set_ap,.disconnect_epoch=epoch,.disconnect_pending=pending,.ctx=f};
    return wifi_ap_shutdown(&ops);
}
int main(void)
{
    /* A disconnect queued after the worker's event drain cancels closing. */
    fixture_t f={.active=true,.pending=true};
    assert(run(&f)==ESP_ERR_NOT_ALLOWED && f.active && f.calls==0);
    assert(service_operation_current()==SERVICE_OPERATION_NONE);
    /* A newly admitted save after that drain must retain its response path. */
    f=(fixture_t){.active=true};
    assert(service_operation_begin(SERVICE_OPERATION_WIFI));
    assert(run(&f)==ESP_ERR_NOT_ALLOWED && f.active && f.calls==0);
    service_operation_end(SERVICE_OPERATION_WIFI);
    /* A disconnect across the actual SDK stop immediately restores the AP. */
    f=(fixture_t){.active=true,.disconnect_during_stop=true};
    assert(run(&f)==ESP_ERR_NOT_ALLOWED && f.active && f.calls==2);
    assert(service_operation_current()==SERVICE_OPERATION_NONE);
    f=(fixture_t){.active=true,.save_during_stop=true};
    assert(run(&f)==ESP_OK && !f.active && !f.admitted);
    assert(service_operation_current()==SERVICE_OPERATION_NONE);
    f=(fixture_t){.active=true,.stop_error=ESP_FAIL};
    assert(run(&f)==ESP_FAIL && f.active);
    assert(service_operation_current()==SERVICE_OPERATION_NONE);
    f=(fixture_t){.active=true,.disconnect_during_stop=true,.restore_error=ESP_FAIL};
    assert(run(&f)==ESP_FAIL && !f.active);
    assert(service_operation_current()==SERVICE_OPERATION_NONE);
    puts("AP shutdown interleavings passed"); return 0;
}
