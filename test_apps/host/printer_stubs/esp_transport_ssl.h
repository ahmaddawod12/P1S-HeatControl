#pragma once
#include "esp_transport.h"
esp_transport_handle_t esp_transport_ssl_init(void);
void esp_transport_ssl_set_cert_data(esp_transport_handle_t, const char *, int);
void esp_transport_ssl_set_common_name(esp_transport_handle_t, const char *);
