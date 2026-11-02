#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
esp_err_t clock_service_wait(const char *ntp_host, uint32_t timeout_ms,
                             bool (*cancelled)(void *), void *ctx);
