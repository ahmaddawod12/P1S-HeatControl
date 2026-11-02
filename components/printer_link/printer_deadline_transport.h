#pragma once
#include "esp_transport.h"
#include <stdbool.h>
#include <stdint.h>
esp_transport_handle_t printer_deadline_transport_create(esp_transport_handle_t,
                                                         uint64_t (*)(void *),
                                                         bool (*)(void *),
                                                         void *);
void printer_deadline_transport_set(esp_transport_handle_t, uint64_t);
void printer_deadline_transport_cleanup(esp_transport_handle_t,
                                        void (*)(void *), void *);
bool printer_socket_nonblocking(int fd);
