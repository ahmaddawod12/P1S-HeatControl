#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "esp_err.h"

#define ESP_TLS_ERR_SSL_WANT_READ (-0x6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-0x6880)

typedef struct esp_tls esp_tls_t;
typedef struct {
    bool keep_alive_enable;
    int keep_alive_idle;
    int keep_alive_interval;
    int keep_alive_count;
} tls_keep_alive_cfg_t;
typedef struct {
    bool non_block;
    const char *common_name;
    esp_err_t (*crt_bundle_attach)(void *);
    tls_keep_alive_cfg_t *keep_alive_cfg;
    int timeout_ms;
} esp_tls_cfg_t;

esp_tls_t *esp_tls_init(void);
int esp_tls_conn_new_async(const char *, int, int, const esp_tls_cfg_t *, esp_tls_t *);
ssize_t esp_tls_conn_read(esp_tls_t *, void *, size_t);
ssize_t esp_tls_conn_write(esp_tls_t *, const void *, size_t);
int esp_tls_conn_destroy(esp_tls_t *);
