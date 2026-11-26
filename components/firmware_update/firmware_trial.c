#include "firmware_update.h"
#include "firmware_trial.h"
#include "app_runtime.h"
#include "device_config.h"
#include "wifi_manager.h"
#include "settings.h"
#include "service_operation.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static atomic_bool s_pending, s_started, s_startup_done, s_cloud_storage_ready;
static atomic_uint s_progress_ms;
static esp_task_wdt_user_handle_t s_watchdog;
static bool s_needs_confirmation, s_requires_marker, s_ota_valid, s_boot_gate;
static bool s_rollback_available, s_trial_reserved;

static const uint8_t *bootstrap_image_hash(void)
{
    _Static_assert(sizeof(((esp_app_desc_t *) 0)->app_elf_sha256) == DEVICE_BOOTSTRAP_HASH_BYTES,
                   "bootstrap marker must store the full ELF hash");
    return esp_app_get_description()->app_elf_sha256;
}

static uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }
bool firmware_trial_pending(void) { return atomic_load(&s_pending); }
void firmware_trial_startup_progress(void) { atomic_store(&s_progress_ms, now_ms()); }
static void detach_watchdog(void)
{ if (s_watchdog) { (void) esp_task_wdt_delete_user(s_watchdog); s_watchdog = NULL; } }
static void release_trial(void)
{
    if (s_trial_reserved) { service_operation_end(SERVICE_OPERATION_TRIAL); s_trial_reserved=false; }
}

static void fail_trial(void)
{
    firmware_update_note_boot_result(FIRMWARE_RESULT_TRIAL_FAILED);
    if (esp_ota_check_rollback_is_possible()) {
        (void) esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();
    }
    /* A sole initial image remains accessible for diagnosis; never mark it
     * invalid and enter a reboot loop without a previous application. */
    firmware_update_note_boot_result(FIRMWARE_RESULT_SETUP_REQUIRED);
    detach_watchdog();
}

void firmware_trial_startup_complete(esp_err_t result)
{
    atomic_store(&s_startup_done, result == ESP_OK);
    firmware_trial_startup_progress();
    if (result != ESP_OK && !atomic_load(&s_started)) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) fail_trial();
    }
}

void firmware_trial_note_cloud_storage_ready(bool ready)
{
    atomic_store(&s_cloud_storage_ready, ready);
}

static void reconcile(void)
{
    firmware_attempt_t attempt;
    const esp_err_t err = device_config_load_attempt(&attempt);
    if (err == ESP_ERR_NOT_FOUND) return;
    if (err != ESP_OK) { firmware_update_note_boot_result(FIRMWARE_RESULT_STORAGE_FAILED); return; }
    if (strcmp(attempt.candidate, custom_app_desc.version) == 0) {
        attempt.phase = FIRMWARE_SUCCEEDED; attempt.result = FIRMWARE_RESULT_INSTALLED;
    } else if (strcmp(attempt.previous, custom_app_desc.version) == 0) {
        if (attempt.phase != FIRMWARE_FAILED) {
            attempt.result = FIRMWARE_RESULT_INTERRUPTED;
            const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
            esp_app_desc_t desc;
            if (invalid && esp_ota_get_partition_description(invalid, &desc) == ESP_OK &&
                strncmp(desc.version, attempt.candidate, sizeof(desc.version)) == 0)
                attempt.result = FIRMWARE_RESULT_ROLLED_BACK;
        }
        attempt.phase = FIRMWARE_FAILED;
    } else return;
    firmware_update_note_boot_result(attempt.result);
    if (device_config_save_attempt(&attempt) != ESP_OK)
        firmware_update_note_boot_result(FIRMWARE_RESULT_STORAGE_FAILED);
}

static void supervisor(void *arg)
{
    (void) arg;
    uint32_t started = now_ms();
    const uint8_t *pilot_image_hash = bootstrap_image_hash();
    bool setup_wait=false;
    while (true) {
        app_status_t status; app_runtime_get_status(&status);
        const uint64_t time = (uint64_t) (esp_timer_get_time() / 1000);
        const bool fresh = status.uptime_ms > 0 && time >= status.uptime_ms && time - status.uptime_ms <= 2000;
        const bool sensor_fresh = status.temperature.timestamp_ms > 0 && time >= status.temperature.timestamp_ms &&
            time - status.temperature.timestamp_ms <= 2000;
        if (s_watchdog && ((uint32_t) (now_ms() - atomic_load(&s_progress_ms)) < 10000 ||
            (fresh && sensor_fresh))) (void) esp_task_wdt_reset_user(s_watchdog);
        device_wifi_credentials_t credentials;
        settings_data_t settings; settings_load_result_t loaded;
        const esp_err_t wifi_record = device_config_load_wifi(&credentials);
        const bool storage = (wifi_record == ESP_OK || wifi_record == ESP_ERR_NOT_FOUND) &&
            settings_load(&settings, &loaded) == ESP_OK && loaded != SETTINGS_LOAD_RECOVERED_CORRUPT;
        /* IDF marks the first empty OTA slot VALID before application startup.
         * Our committed image marker records that the pilot passed its
         * local checks. Once present, ordinary boots need no router. */
        if (s_needs_confirmation && s_requires_marker && s_ota_valid &&
            device_config_bootstrap_confirmed(pilot_image_hash) == ESP_OK) {
            s_needs_confirmation = false;
            started = now_ms();
        }
        if (!s_needs_confirmation) {
            if (s_boot_gate) {
                if (!fresh || app_runtime_end_maintenance() != ESP_OK) {
                    if (now_ms() - started >= 120000) {
                        firmware_update_note_boot_result(FIRMWARE_RESULT_UNSAFE); break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(250)); continue;
                }
                s_boot_gate = false;
                detach_watchdog();
            }
            if (storage) { reconcile(); break; }
            if (now_ms() - started >= 120000) break;
        } else {
            firmware_health_t health = {.maintenance_ready = app_runtime_maintenance_ready(),
                .storage_ready = storage, .runtime_fresh = fresh, .sensor_fresh = sensor_fresh,
                .fatal = status.fault == HEAT_FAULT_OUTPUT_INIT || status.fault == HEAT_FAULT_NVS};
            wifi_manager_status_t wifi; wifi_manager_get_status(&wifi);
            health.local_services_ready = atomic_load(&s_startup_done);
            health.cloud_storage_ready = atomic_load(&s_cloud_storage_ready);
            health.recovery_ready = wifi.setup_mode == WIFI_SETUP_NONE
                ? wifi.connected : wifi.setup_portal_ready;
            const firmware_trial_action_t action = firmware_trial_evaluate_boot(&health,
                now_ms() - started, true, s_rollback_available);
            if (action == FIRMWARE_TRIAL_CONFIRM) {
                const esp_partition_t *running = esp_ota_get_running_partition();
                esp_ota_img_states_t state;
                esp_err_t err = s_requires_marker ? device_config_confirm_bootstrap(pilot_image_hash) : ESP_OK;
                if (err != ESP_OK) { fail_trial(); break; }
                err = esp_ota_get_state_partition(running, &state);
                if (err != ESP_OK || state == ESP_OTA_IMG_UNDEFINED) err = esp_ota_set_boot_partition(running);
                if (err == ESP_OK) err = esp_ota_mark_app_valid_cancel_rollback();
                if (err != ESP_OK) { fail_trial(); break; }
                /* Give the confirmed image time to release maintenance if
                 * admission is temporarily busy after the OTA writes. */
                s_needs_confirmation = false;
                started = now_ms();
                firmware_update_note_boot_result(FIRMWARE_RESULT_INSTALLED);
            }
            if (action == FIRMWARE_TRIAL_REJECT) { fail_trial(); break; }
            if (action == FIRMWARE_TRIAL_SETUP_WAIT && !setup_wait) {
                firmware_update_note_boot_result(FIRMWARE_RESULT_SETUP_REQUIRED);
                detach_watchdog();
                setup_wait=true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    detach_watchdog();
    if (!s_needs_confirmation) atomic_store(&s_pending, false);
    release_trial();
    vTaskDelete(NULL);
}

esp_err_t firmware_trial_start(void)
{
    if (atomic_exchange(&s_started, true)) return ESP_OK;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t read = running ? esp_ota_get_state_partition(running, &state) : ESP_ERR_NOT_FOUND;
#ifdef CONFIG_P1S_FIREBASE_PUBLIC_RELEASE
    s_requires_marker = true;
#else
    s_requires_marker = firmware_boot_requires_marker(custom_app_desc.image_flags);
#endif
    s_ota_valid = read == ESP_OK && state == ESP_OTA_IMG_VALID;
    s_needs_confirmation = firmware_boot_needs_confirmation(s_ota_valid, s_requires_marker,
                                                            bootstrap_image_hash());
    s_boot_gate = s_needs_confirmation;
    s_rollback_available=esp_ota_check_rollback_is_possible();
    atomic_store(&s_pending, s_needs_confirmation);
    firmware_trial_startup_progress();
    if (s_needs_confirmation) {
        (void) app_runtime_set_boot_maintenance(true);
        if (s_rollback_available) {
            s_trial_reserved=service_operation_begin(SERVICE_OPERATION_TRIAL);
            if (!s_trial_reserved) { fail_trial(); return ESP_OK; }
        }
        const esp_task_wdt_config_t config = {.timeout_ms = 12000,
            .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U, .trigger_panic = true};
        esp_err_t err = esp_task_wdt_reconfigure(&config);
        if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&config);
        if (err != ESP_OK || esp_task_wdt_add_user("ota_trial", &s_watchdog) != ESP_OK) {
            fail_trial(); return ESP_OK;
        }
    }
    if (xTaskCreate(supervisor, "ota_trial", 6144, NULL, 4, NULL) != pdPASS) {
        if (s_needs_confirmation) fail_trial();
        else firmware_update_note_boot_result(FIRMWARE_RESULT_STORAGE_FAILED);
    }
    return ESP_OK;
}
