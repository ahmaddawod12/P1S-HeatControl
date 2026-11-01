#pragma once
#include "web_server.h"
#include "esp_http_server.h"
esp_err_t wifi_api_register(httpd_handle_t server,const web_server_ops_t *ops);
