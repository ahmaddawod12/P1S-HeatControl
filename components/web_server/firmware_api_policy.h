#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool firmware_request_origin_valid(const char *host, const char *origin, const char *local_ip, uint16_t port);
bool firmware_request_body_valid(const char *body, size_t length);
