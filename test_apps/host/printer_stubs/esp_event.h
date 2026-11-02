#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
#define ESP_EVENT_ANY_ID (-1)
extern const char *WIFI_EVENT;
extern const char *IP_EVENT;
#define IP_EVENT_STA_GOT_IP 1
#define IP_EVENT_STA_LOST_IP 2
esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t,
                                              esp_event_handler_t, void *,
                                              esp_event_handler_instance_t *);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t, int32_t,
                                                esp_event_handler_instance_t);
