#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../printer_stubs/esp_http_server.h"
typedef struct {
    uint16_t server_port;
    unsigned max_open_sockets, max_uri_handlers;
    bool lru_purge_enable;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTPD_SOCK_ERR_TIMEOUT (-3)
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_stop(httpd_handle_t);
int httpd_req_to_sockfd(httpd_req_t *);
