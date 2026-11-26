#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

void http_csrf_init(void);
const char *http_csrf_token(void);
bool http_csrf_request_trusted(httpd_req_t *req);
