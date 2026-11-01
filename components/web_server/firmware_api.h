#pragma once
#include "web_server.h"
#include "esp_http_server.h"
esp_err_t firmware_api_register(httpd_handle_t server, const web_server_ops_t *ops);
bool firmware_api_request_trusted(httpd_req_t *req);
const char *firmware_api_csrf_token(void);
