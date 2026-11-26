#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
esp_err_t clock_service_wait(const char *ntp_host, uint32_t timeout_ms,
                             bool (*cancelled)(void *), void *ctx);

/* Wait for a new confirmed SNTP sync even if the wall clock is already set. */
esp_err_t clock_service_sync(const char *ntp_host, uint32_t timeout_ms);
/* Measured against monotonic uptime so wall-clock corrections cannot extend it. */
bool clock_service_is_recent(uint32_t max_age_seconds);
bool clock_service_has_valid_wall_time(void);
