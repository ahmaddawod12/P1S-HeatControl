#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "firmware_types.h"
typedef struct {
    bool maintenance_ready, storage_ready, runtime_fresh, sensor_fresh, fatal;
    bool local_services_ready, cloud_storage_ready, recovery_ready;
} firmware_health_t;
typedef enum { FIRMWARE_TRIAL_WAIT, FIRMWARE_TRIAL_CONFIRM, FIRMWARE_TRIAL_REJECT,
    FIRMWARE_TRIAL_SETUP_WAIT } firmware_trial_action_t;
firmware_trial_action_t firmware_trial_evaluate(const firmware_health_t *health, uint32_t elapsed_ms, bool pending);
firmware_trial_action_t firmware_trial_evaluate_boot(const firmware_health_t *health, uint32_t elapsed_ms,
    bool pending, bool rollback_available);
static inline bool firmware_boot_requires_marker(uint32_t image_flags)
{
    return (image_flags & (FIRMWARE_IMAGE_PERSONAL | FIRMWARE_IMAGE_FIREBASE_PILOT)) != 0;
}
bool firmware_boot_needs_confirmation(bool ota_valid, bool requires_marker,
                                      const uint8_t *bootstrap_image_hash);
