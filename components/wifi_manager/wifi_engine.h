#pragma once
#include "wifi_manager.h"

typedef struct {
    esp_err_t (*save)(const wifi_credentials_t *,void *);
    esp_err_t (*set_ap)(bool,void *);
    esp_err_t (*connect)(const wifi_credentials_t *,void *);
    void *ctx;
} wifi_engine_ops_t;
typedef struct {
    wifi_manager_status_t status;
    uint64_t offline_since, retry_at, ap_retry_at, close_ap_at;
    size_t retry_attempt;
    bool storage_ready, ap_recovery_needed;
} wifi_engine_t;
void wifi_engine_init(wifi_engine_t *,const wifi_credentials_t *,esp_err_t,uint64_t);
esp_err_t wifi_engine_apply(wifi_engine_t *,const wifi_credentials_t *,uint64_t,const wifi_engine_ops_t *);
void wifi_engine_tick(wifi_engine_t *,uint64_t,bool,const wifi_engine_ops_t *);
void wifi_engine_connected(wifi_engine_t *,const char *,const char *,int8_t,uint64_t);
void wifi_engine_disconnected(wifi_engine_t *,wifi_result_t,uint64_t);
