#include "firmware_trial.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    /* IDF initializes an empty first OTA slot as VALID before app startup. */
    assert(firmware_boot_needs_confirmation(true, true, false));
    assert(!firmware_boot_needs_confirmation(true, true, true));
    assert(!firmware_boot_needs_confirmation(true, false, false));
    assert(firmware_boot_needs_confirmation(false, false, true));
    firmware_health_t h = {.outputs_safe = true, .storage_ready = true,
        .runtime_fresh = true, .sensor_fresh = true, .wifi_ready = true, .http_ready = true};
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_CONFIRM);
    h.wifi_ready = false;
    assert(firmware_trial_evaluate(&h, 119999, true) == FIRMWARE_TRIAL_WAIT);
    assert(firmware_trial_evaluate(&h, 120000, true) == FIRMWARE_TRIAL_REJECT);
    assert(firmware_trial_evaluate(&h, 900000, false) == FIRMWARE_TRIAL_WAIT);
    h.wifi_ready = true; h.http_ready = false;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_WAIT);
    h.http_ready = true; h.sensor_fresh = false;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_WAIT);
    h.sensor_fresh = true; h.outputs_safe = false;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_WAIT);
    h.outputs_safe = true; h.fatal = true;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_REJECT);
    h.fatal = false; h.wifi_ready = false;
    assert(firmware_trial_evaluate_boot(&h, 120000, true, true) == FIRMWARE_TRIAL_REJECT);
    assert(firmware_trial_evaluate_boot(&h, 120000, true, false) == FIRMWARE_TRIAL_SETUP_WAIT);
    /* An AP-only connection is still setup, including after the deadline. */
    assert(firmware_trial_evaluate_boot(&h, 300000, true, false) == FIRMWARE_TRIAL_SETUP_WAIT);
    h.wifi_ready = true;
    assert(firmware_trial_evaluate_boot(&h, 300001, true, false) == FIRMWARE_TRIAL_CONFIRM);
    h.wifi_ready = false;
    assert(firmware_trial_evaluate_boot(&h, 900000, false, true) == FIRMWARE_TRIAL_WAIT);
    puts("firmware trial passed");
    return 0;
}
