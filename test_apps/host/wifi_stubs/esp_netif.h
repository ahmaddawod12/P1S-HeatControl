#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct esp_netif esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct {
    esp_netif_t *esp_netif;
    esp_netif_ip_info_t ip_info;
    bool ip_changed;
} ip_event_got_ip_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (unsigned)((ip)->addr & 255), (unsigned)(((ip)->addr >> 8) & 255), \
    (unsigned)(((ip)->addr >> 16) & 255), (unsigned)(((ip)->addr >> 24) & 255)
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
