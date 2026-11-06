#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

/* Read one complete, NUL-free body within five seconds, including socket waits.
 * capacity includes the terminating NUL. Restore the session's receive timeout. */
esp_err_t http_request_receive_body(httpd_req_t *req, char *body, size_t capacity);

/* Return failure after sending so IDF closes instead of draining unread input. */
esp_err_t http_request_close_json(httpd_req_t *req, const char *status, const char *json);
bool http_request_require_empty_body(httpd_req_t *req);
