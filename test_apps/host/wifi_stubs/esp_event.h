#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
#define WIFI_EVENT "wifi"
#define IP_EVENT "ip"
#define ESP_EVENT_ANY_ID (-1)
#define IP_EVENT_STA_GOT_IP 0
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t,
    esp_event_handler_t, void *, void *);
