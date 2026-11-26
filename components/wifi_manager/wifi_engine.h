#pragma once
#include "wifi_manager.h"

typedef struct {
    esp_err_t (*save)(const wifi_credentials_t *,void *);
    /* Update active whenever the radio mode changes, even during recovery. */
    esp_err_t (*set_ap)(bool enabled, bool *active, void *ctx);
    esp_err_t (*connect)(const wifi_credentials_t *,void *);
    void *ctx;
} wifi_engine_ops_t;
typedef struct {
    wifi_manager_status_t status;
    char password[WIFI_MANAGER_PASSWORD_LEN];
    uint64_t retry_at, ap_retry_at;
    size_t retry_attempt;
    bool storage_ready;
} wifi_engine_t;

void wifi_engine_init(wifi_engine_t *,const wifi_credentials_t *,esp_err_t,uint64_t);
esp_err_t wifi_engine_apply(wifi_engine_t *,const wifi_credentials_t *,uint64_t,const wifi_engine_ops_t *);
wifi_setup_mode_t wifi_setup_mode(bool connected, bool paired);
void wifi_engine_tick(wifi_engine_t *,uint64_t,bool,const wifi_engine_ops_t *);
void wifi_engine_connected(wifi_engine_t *,const char *,const char *,int8_t);
void wifi_engine_disconnected(wifi_engine_t *,wifi_result_t,uint64_t);
