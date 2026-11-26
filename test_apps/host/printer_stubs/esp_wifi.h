#pragma once
#include <stdint.h>
#include "esp_err.h"
#define WIFI_EVENT_STA_DISCONNECTED 5
#define WIFI_IF_STA 0
esp_err_t esp_wifi_get_mac(int interface, uint8_t mac[6]);
