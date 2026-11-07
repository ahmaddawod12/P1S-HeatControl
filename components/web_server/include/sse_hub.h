#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#define SSE_HUB_MAX_CLIENTS 4

/* Call from serialized server startup before HTTP/broadcast tasks can use the
 * hub. Initialization is retained across server stop/start cycles. */
esp_err_t sse_hub_init(void);

/* Detaches `req` and keeps its socket open as an event stream. Returns
 * ESP_ERR_NO_MEM once SSE_HUB_MAX_CLIENTS streams are already registered. */
esp_err_t sse_hub_add_client(httpd_req_t *req);

/* Sends `frame` to every registered client, dropping and completing any
 * client whose socket errors. */
void sse_hub_broadcast(const char *frame, size_t length);

size_t sse_hub_client_count(void);
void sse_hub_close_all(void);
