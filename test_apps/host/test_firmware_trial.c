#include "firmware_trial.h"
#include "device_config.h"
#include <assert.h>
#include <stdio.h>

static esp_err_t config_init_result = ESP_OK;
static bool config_ready, bootstrap_marker;
static const uint8_t *confirmed_image;
static unsigned config_inits;

esp_err_t device_config_init(void)
{
    ++config_inits;
    config_ready = config_init_result == ESP_OK;
    return config_init_result;
}

esp_err_t device_config_bootstrap_confirmed(const uint8_t *pilot_image_hash)
{
    assert(config_ready);
    return bootstrap_marker && confirmed_image == pilot_image_hash ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void boot_gate_uses_the_image_confirmation(void)
{
    const uint8_t pilot_image[DEVICE_BOOTSTRAP_HASH_BYTES] = {1};
    const uint8_t other_image[DEVICE_BOOTSTRAP_HASH_BYTES] = {2};
    assert(firmware_boot_needs_confirmation(false, false, NULL));
    assert(!firmware_boot_needs_confirmation(true, false, NULL));
    assert(config_inits == 0);

    /* IDF initializes an empty first slot as VALID before app startup. */
    assert(firmware_boot_requires_marker(FIRMWARE_IMAGE_PERSONAL));
    assert(firmware_boot_needs_confirmation(true, true, NULL));
    bootstrap_marker = true;
    config_ready = false; /* Simulate a bootstrap confirmation without an image hash. */
    assert(!firmware_boot_needs_confirmation(true, true, NULL));

    assert(firmware_boot_requires_marker(FIRMWARE_IMAGE_FIREBASE_PILOT));
    assert(firmware_boot_needs_confirmation(true, true, pilot_image));
    confirmed_image = pilot_image;
    config_ready = false;
    assert(!firmware_boot_needs_confirmation(true, true, pilot_image));
    assert(firmware_boot_needs_confirmation(true, true, other_image));
    assert(firmware_boot_needs_confirmation(false, true, pilot_image));
    config_init_result = ESP_FAIL;
    assert(firmware_boot_needs_confirmation(true, true, pilot_image));
}

int main(void)
{
    boot_gate_uses_the_image_confirmation();
    firmware_health_t h = {.maintenance_ready = true, .storage_ready = true,
        .runtime_fresh = true, .sensor_fresh = true, .local_services_ready = true,
        .cloud_storage_ready = true, .recovery_ready = true};
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_CONFIRM);
    h.recovery_ready = false;
    assert(firmware_trial_evaluate(&h, 119999, true) == FIRMWARE_TRIAL_WAIT);
    assert(firmware_trial_evaluate(&h, 120000, true) == FIRMWARE_TRIAL_REJECT);
    assert(firmware_trial_evaluate(&h, 900000, false) == FIRMWARE_TRIAL_WAIT);
    h.recovery_ready = true;
    h.sensor_fresh = false;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_WAIT);
    h.sensor_fresh = true; h.maintenance_ready = false;
    /* Fresh telemetry can show the relay off while its shutdown save is pending. */
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_WAIT);
    assert(firmware_trial_evaluate_boot(&h, 1000, true, false) == FIRMWARE_TRIAL_WAIT);
    h.maintenance_ready = true;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_CONFIRM);
    h.fatal = true;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_REJECT);
    h.fatal = false;
    h.recovery_ready = false;
    assert(firmware_trial_evaluate_boot(&h, 120000, true, true) == FIRMWARE_TRIAL_REJECT);
    assert(firmware_trial_evaluate_boot(&h, 120000, true, false) == FIRMWARE_TRIAL_SETUP_WAIT);
    /* The initial image can continue waiting for recovery after the deadline. */
    assert(firmware_trial_evaluate_boot(&h, 300000, true, false) == FIRMWARE_TRIAL_SETUP_WAIT);
    h.recovery_ready = true;
    assert(firmware_trial_evaluate_boot(&h, 300001, true, false) == FIRMWARE_TRIAL_CONFIRM);
    assert(firmware_trial_evaluate_boot(&h, 900000, false, true) == FIRMWARE_TRIAL_WAIT);
    h.local_services_ready = false;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_WAIT);
    h.local_services_ready = true;
    /* A portal alone cannot recover an unreadable enrollment record. */
    h.cloud_storage_ready = false;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_WAIT);
    assert(firmware_trial_evaluate_boot(&h, 120000, true, true) == FIRMWARE_TRIAL_REJECT);
    assert(firmware_trial_evaluate_boot(&h, 120000, true, false) == FIRMWARE_TRIAL_SETUP_WAIT);
    /* Healthy local storage and a working portal suffice during an outage. */
    h.cloud_storage_ready = true;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_CONFIRM);
    h.sensor_fresh = false;
    assert(firmware_trial_evaluate(&h, 1000, true) == FIRMWARE_TRIAL_WAIT);
    puts("firmware trial passed");
    return 0;
}
