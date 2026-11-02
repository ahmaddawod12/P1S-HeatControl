#pragma once
#include "esp_http_server.h"
#include "web_server.h"
esp_err_t printer_api_register(httpd_handle_t, const web_server_ops_t *);
