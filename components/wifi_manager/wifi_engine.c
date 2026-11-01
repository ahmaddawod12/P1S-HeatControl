#include "wifi_engine.h"
#include <string.h>

#define FALLBACK_MS 30000U
#define AP_GRACE_MS 10000U

static void set_credentials(wifi_engine_t *e,const wifi_credentials_t *c)
{
    memcpy(e->status.ssid,c->ssid,sizeof(c->ssid));
    memcpy(e->status.password,c->password,sizeof(c->password));
    e->status.configured=true;
}
void wifi_engine_init(wifi_engine_t *e,const wifi_credentials_t *c,esp_err_t error,uint64_t now)
{
    memset(e,0,sizeof(*e));
    e->storage_ready=error==ESP_OK;
    e->offline_since=now; e->retry_at=UINT64_MAX; e->close_ap_at=UINT64_MAX;
    strcpy(e->status.ap_ssid,WIFI_MANAGER_AP_SSID);
    strcpy(e->status.ap_ip,WIFI_MANAGER_AP_IP); strcpy(e->status.ip_addr,"0.0.0.0");
    e->status.result=e->storage_ready?WIFI_RESULT_SETUP_REQUIRED:WIFI_RESULT_STORAGE_FAILED;
    if (c && e->storage_ready) {
        set_credentials(e,c); e->retry_at=now; e->status.result=WIFI_RESULT_CONNECTING;
    }
}
static esp_err_t set_ap(wifi_engine_t *e,bool enabled,uint64_t now,const wifi_engine_ops_t *o)
{
    if (e->status.ap_active==enabled) return ESP_OK;
    const esp_err_t err=o->set_ap(enabled,o->ctx);
    if (err==ESP_OK) {
        e->status.ap_active=enabled; e->ap_retry_at=0;
        if (enabled) e->ap_recovery_needed=false;
        if (e->status.result==WIFI_RESULT_AP_FAILED) e->status.result=
            e->status.connected?WIFI_RESULT_CONNECTED:
            e->status.configured?WIFI_RESULT_CONNECTING:WIFI_RESULT_SETUP_REQUIRED;
    } else if (!enabled && err==ESP_ERR_NOT_ALLOWED) {
        /* Shutdown lost admission or recovered a late disconnect: AP stayed
         * available. The next event drain decides whether to close later. */
        return err;
    } else {
        if (!enabled) { e->status.ap_active=false; e->ap_recovery_needed=true; }
        e->ap_retry_at=now+1000; e->status.result=WIFI_RESULT_AP_FAILED;
    }
    return err;
}
void wifi_engine_disconnected(wifi_engine_t *e,wifi_result_t result,uint64_t now)
{
    if (e->status.connected) e->offline_since=now;
    e->status.connected=false; e->status.rssi=0;
    strcpy(e->status.ip_addr,"0.0.0.0"); e->close_ap_at=UINT64_MAX;
    if (e->status.configured) {
        e->status.result=result;
        e->retry_at=now+wifi_manager_backoff_ms(e->retry_attempt);
        if (e->retry_attempt<SIZE_MAX) ++e->retry_attempt;
    }
}
static esp_err_t connect(wifi_engine_t *e,uint64_t now,const wifi_engine_ops_t *o)
{
    wifi_credentials_t c;
    memcpy(c.ssid,e->status.ssid,sizeof(c.ssid));
    memcpy(c.password,e->status.password,sizeof(c.password));
    e->retry_at=UINT64_MAX;
    const esp_err_t err=o->connect(&c,o->ctx);
    if (err!=ESP_OK) wifi_engine_disconnected(e,WIFI_RESULT_DRIVER_FAILED,now);
    return err;
}
esp_err_t wifi_engine_apply(wifi_engine_t *e,const wifi_credentials_t *c,uint64_t now,const wifi_engine_ops_t *o)
{
    if (!e->storage_ready) return ESP_ERR_INVALID_STATE;
    if (!c || !wifi_credentials_are_valid_n(c->ssid,sizeof(c->ssid),c->password,sizeof(c->password)))
        return ESP_ERR_INVALID_ARG;
    e->close_ap_at=UINT64_MAX;
    esp_err_t err=set_ap(e,true,now,o);
    if (err!=ESP_OK) return err;
    err=o->save(c,o->ctx);
    if (err!=ESP_OK) { e->status.result=WIFI_RESULT_SAVE_FAILED; return err; }
    set_credentials(e,c);
    e->status.connected=false; e->status.rssi=0;
    strcpy(e->status.ip_addr,"0.0.0.0");
    e->status.result=WIFI_RESULT_CONNECTING; e->offline_since=now; e->retry_attempt=0;
    return connect(e,now,o);
}
void wifi_engine_tick(wifi_engine_t *e,uint64_t now,bool can_close_ap,const wifi_engine_ops_t *o)
{
    const bool need_ap=e->ap_recovery_needed || !e->status.configured ||
        (!e->status.connected && now-e->offline_since>=FALLBACK_MS);
    if (need_ap && !e->status.ap_active && now>=e->ap_retry_at) (void)set_ap(e,true,now,o);
    if (e->status.connected && e->status.ap_active && can_close_ap && now>=e->close_ap_at && now>=e->ap_retry_at)
        (void)set_ap(e,false,now,o);
    if (e->status.configured && !e->status.connected && now>=e->retry_at) (void)connect(e,now,o);
}
void wifi_engine_connected(wifi_engine_t *e,const char *ssid,const char *ip,int8_t rssi,uint64_t now)
{
    if (!e->status.configured || strcmp(ssid,e->status.ssid)!=0) return;
    e->status.connected=true; e->status.rssi=rssi;
    strncpy(e->status.ip_addr,ip,sizeof(e->status.ip_addr)-1);
    e->status.ip_addr[sizeof(e->status.ip_addr)-1]=0;
    e->status.result=WIFI_RESULT_CONNECTED; e->retry_attempt=0; e->retry_at=UINT64_MAX;
    e->close_ap_at=now+AP_GRACE_MS;
}
