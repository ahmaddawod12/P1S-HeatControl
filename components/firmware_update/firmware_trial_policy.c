#include "firmware_trial.h"
#include "device_config.h"
firmware_trial_action_t firmware_trial_evaluate_boot(const firmware_health_t *h, uint32_t elapsed,
    bool pending, bool rollback_available)
{
    if (rollback_available) return firmware_trial_evaluate(h,elapsed,pending);
    if (!pending) return FIRMWARE_TRIAL_WAIT;
    if (!h || h->fatal) return FIRMWARE_TRIAL_REJECT;
    const firmware_trial_action_t health=firmware_trial_evaluate(h,0,true);
    if (health==FIRMWARE_TRIAL_CONFIRM) return health;
    return elapsed>=120000 ? FIRMWARE_TRIAL_SETUP_WAIT : FIRMWARE_TRIAL_WAIT;
}
bool firmware_boot_needs_confirmation(bool ota_valid, bool requires_marker,
                                      const uint8_t *bootstrap_image_hash)
{
    if (!ota_valid) return true;
    if (!requires_marker) return false;
    /* Read the dedicated partition before starting the runtime in maintenance. */
    return device_config_init() != ESP_OK ||
        device_config_bootstrap_confirmed(bootstrap_image_hash) != ESP_OK;
}
firmware_trial_action_t firmware_trial_evaluate(const firmware_health_t *h, uint32_t elapsed_ms, bool pending)
{
    if (!pending) return FIRMWARE_TRIAL_WAIT;
    if (!h || h->fatal || elapsed_ms >= 120000) return FIRMWARE_TRIAL_REJECT;
    const bool services_ready = h->local_services_ready && h->cloud_storage_ready &&
        h->recovery_ready;
    return h->maintenance_ready && h->storage_ready && h->runtime_fresh && h->sensor_fresh &&
        services_ready ? FIRMWARE_TRIAL_CONFIRM : FIRMWARE_TRIAL_WAIT;
}
