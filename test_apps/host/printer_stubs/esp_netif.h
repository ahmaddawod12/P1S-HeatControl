#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef struct esp_netif esp_netif_t;
typedef struct {
  uint32_t addr;
} esp_ip4_addr_t;
typedef struct {
  esp_ip4_addr_t ip, netmask, gw;
} esp_netif_ip_info_t;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
