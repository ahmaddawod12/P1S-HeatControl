#include "firmware_trial.h"
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
bool firmware_boot_needs_confirmation(bool ota_valid, bool personal, bool bootstrap_confirmed)
{
    return !ota_valid || (personal && !bootstrap_confirmed);
}
firmware_trial_action_t firmware_trial_evaluate(const firmware_health_t *h, uint32_t elapsed_ms, bool pending)
{
    if (!pending) return FIRMWARE_TRIAL_WAIT;
    if (!h || h->fatal || elapsed_ms >= 120000) return FIRMWARE_TRIAL_REJECT;
    return h->outputs_safe && h->storage_ready && h->runtime_fresh && h->sensor_fresh &&
        h->wifi_ready && h->http_ready ? FIRMWARE_TRIAL_CONFIRM : FIRMWARE_TRIAL_WAIT;
}
