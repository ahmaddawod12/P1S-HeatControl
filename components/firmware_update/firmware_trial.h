#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool outputs_safe, storage_ready, runtime_fresh, sensor_fresh, wifi_ready, http_ready, fatal;
} firmware_health_t;
typedef enum { FIRMWARE_TRIAL_WAIT, FIRMWARE_TRIAL_CONFIRM, FIRMWARE_TRIAL_REJECT,
    FIRMWARE_TRIAL_SETUP_WAIT } firmware_trial_action_t;
firmware_trial_action_t firmware_trial_evaluate(const firmware_health_t *health, uint32_t elapsed_ms, bool pending);
firmware_trial_action_t firmware_trial_evaluate_boot(const firmware_health_t *health, uint32_t elapsed_ms,
    bool pending, bool rollback_available);
bool firmware_boot_needs_confirmation(bool ota_valid, bool personal, bool bootstrap_confirmed);
