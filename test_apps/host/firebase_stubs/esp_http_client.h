#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define ESP_ERR_HTTP_EAGAIN 0x7007

typedef enum { HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_GET } esp_http_client_method_t;
typedef struct http_fixture *esp_http_client_handle_t;
struct fake_transport;
typedef struct {
    const char *url;
    esp_http_client_method_t method;
    esp_err_t (*crt_bundle_attach)(void *);
    int timeout_ms;
    int buffer_size_tx;
    bool disable_auto_redirect;
    bool keep_alive_enable;
    struct fake_transport *transport;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int length);
int esp_http_client_write(esp_http_client_handle_t client, const char *body, int length);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int capacity);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client);
esp_err_t esp_http_client_get_url(esp_http_client_handle_t client, char *url, int capacity);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms);
