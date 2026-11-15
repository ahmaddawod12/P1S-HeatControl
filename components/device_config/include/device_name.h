#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define DEVICE_NAME_LEN 23U
#define DEVICE_HOSTNAME_LEN 65U
#define DEVICE_DEFAULT_NAME "1"

/* Empty selects p1s-1. Nonempty names use lowercase ASCII DNS labels. */
bool device_name_valid_n(const char *name, size_t capacity);
esp_err_t device_name_normalize(const char *name, char out[DEVICE_NAME_LEN]);
esp_err_t device_hostname(const char *name, char hostname[DEVICE_HOSTNAME_LEN]);
esp_err_t device_name_next(const char *name, char next[DEVICE_NAME_LEN]);
