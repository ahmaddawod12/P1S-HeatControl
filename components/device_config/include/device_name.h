#pragma once

#include <stdbool.h>
#include <stddef.h>

#define DEVICE_NAME_LEN 23U

/* Validate stored names, including the empty name used by older records. */
bool device_name_valid_n(const char *name, size_t capacity);
