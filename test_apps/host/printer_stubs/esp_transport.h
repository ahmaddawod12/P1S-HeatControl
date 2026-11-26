#pragma once
#include "esp_err.h"
enum {
    ERR_TCP_TRANSPORT_CONNECTION_FAILED = -2,
    ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN = -1,
    ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT = 0,
};
typedef struct fake_transport *esp_transport_handle_t;
typedef int (*connect_func)(esp_transport_handle_t, const char *, int, int);
typedef int (*io_read_func)(esp_transport_handle_t, char *, int, int);
typedef int (*io_func)(esp_transport_handle_t, const char *, int, int);
typedef int (*trans_func)(esp_transport_handle_t);
typedef int (*poll_func)(esp_transport_handle_t, int);
esp_transport_handle_t esp_transport_init(void);
esp_err_t esp_transport_destroy(esp_transport_handle_t);
int esp_transport_connect(esp_transport_handle_t, const char *, int, int);
int esp_transport_read(esp_transport_handle_t, char *, int, int);
int esp_transport_write(esp_transport_handle_t, const char *, int, int);
int esp_transport_poll_read(esp_transport_handle_t, int);
int esp_transport_poll_write(esp_transport_handle_t, int);
int esp_transport_close(esp_transport_handle_t);
int esp_transport_get_socket(esp_transport_handle_t);
void *esp_transport_get_context_data(esp_transport_handle_t);
esp_err_t esp_transport_set_context_data(esp_transport_handle_t, void *);
esp_err_t esp_transport_set_default_port(esp_transport_handle_t, int);
esp_err_t esp_transport_set_func(esp_transport_handle_t, connect_func,
                                 io_read_func, io_func, trans_func, poll_func,
                                 poll_func, trans_func);
