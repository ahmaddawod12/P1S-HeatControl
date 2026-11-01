#pragma once
#include <stdint.h>
#include "esp_err.h"

enum { WIFI_EVENT_STA_CONNECTED, WIFI_EVENT_STA_DISCONNECTED };
enum {
    WIFI_REASON_AUTH_EXPIRE = 2, WIFI_REASON_ASSOC_LEAVE = 8,
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15, WIFI_REASON_NO_AP_FOUND = 201,
    WIFI_REASON_AUTH_FAIL = 202, WIFI_REASON_HANDSHAKE_TIMEOUT = 204
};
typedef enum { WIFI_MODE_STA = 1, WIFI_MODE_APSTA = 3 } wifi_mode_t;
enum { WIFI_IF_STA, WIFI_IF_AP, WIFI_STORAGE_RAM, WIFI_AUTH_OPEN };
typedef struct { uint8_t ssid[33]; int8_t rssi; } wifi_ap_record_t;
typedef struct {
    uint8_t ssid[32], ssid_len, bssid[6], reason;
    int8_t rssi;
} wifi_event_sta_disconnected_t;
typedef union {
    struct { uint8_t ssid[32], password[64]; } sta;
    struct { uint8_t ssid[32], ssid_len, channel, authmode, max_connection; } ap;
} wifi_config_t;
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() {0}
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(wifi_mode_t);
esp_err_t esp_wifi_get_config(int, wifi_config_t *);
esp_err_t esp_wifi_set_config(int, const wifi_config_t *);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
