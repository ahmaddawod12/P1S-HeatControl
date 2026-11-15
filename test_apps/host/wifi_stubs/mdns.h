#pragma once
#include "esp_err.h"
#include "esp_netif.h"
#include <stddef.h>
enum { MDNS_TYPE_A=1 };
typedef struct mdns_result_s {
    struct mdns_result_s *next;
    esp_netif_t *esp_netif;
} mdns_result_t;
esp_err_t mdns_init(void);
esp_err_t mdns_hostname_set(const char *);
esp_err_t mdns_hostname_get(char *);
esp_err_t mdns_instance_name_set(const char *);
esp_err_t mdns_service_add(const char *, const char *, const char *, unsigned,
    const void *, unsigned);
esp_err_t mdns_query(const char *,const char *,const char *,uint16_t,uint32_t,size_t,mdns_result_t **);
void mdns_query_results_free(mdns_result_t *);
