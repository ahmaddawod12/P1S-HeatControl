#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_transport.h"
#include "firebase_protocol.h"

/* The request line must hold a full ID token, path and query. Leave room
 * for the remaining URL fields and request headers. */
#define FIREBASE_HTTP_TX_BUFFER_BYTES (FIREBASE_ID_TOKEN_BYTES + 1024U)

typedef struct {
    esp_http_client_handle_t http;
    esp_transport_handle_t transport;
    esp_tls_t *tls;
    esp_err_t connect_error;
    int64_t deadline_us;
    bool keep_alive;
} firebase_https_connection_t;

/* Owns both handles until cleanup. Each transport call uses deadline_us,
 * so one deadline can cover multiple ESP HTTP operations and redirects. */
esp_err_t firebase_https_init(firebase_https_connection_t *connection,
                             esp_http_client_method_t method, const char *url, int budget_ms);
void firebase_https_cleanup(firebase_https_connection_t *connection);

esp_err_t firebase_https_request(esp_http_client_method_t method, const char *url,
                                const char *content_type, const char *body,
                                char *response, size_t capacity, int *status);
