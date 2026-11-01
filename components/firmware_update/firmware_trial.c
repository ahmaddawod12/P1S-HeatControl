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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdatomic.h>
#include <string.h>

static atomic_bool s_pending, s_started, s_startup_done;
static atomic_uint s_progress_ms;
static esp_task_wdt_user_handle_t s_watchdog;
static bool s_needs_confirmation, s_personal, s_ota_valid, s_boot_gate;
static bool s_rollback_available, s_trial_reserved;
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
    atomic_store(&s_startup_done, true);
    firmware_trial_startup_progress();
    if (result != ESP_OK && !atomic_load(&s_started)) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) fail_trial();
    }
}

static bool http_alive(void)
{
    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;
    const struct timeval timeout = {.tv_sec = 1};
    (void) setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void) setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(80)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static const char request[] = "GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    char response[64] = {0};
    bool ok = connect(sock, (struct sockaddr *) &address, sizeof(address)) == 0 &&
        send(sock, request, sizeof(request) - 1, 0) == sizeof(request) - 1;
    size_t used = 0;
    while (ok && used < 12) {
        const int n = recv(sock, response + used, sizeof(response) - 1 - used, 0);
        if (n <= 0) { ok = false; break; }
        used += (size_t) n;
    }
    close(sock);
    return ok && strncmp(response, "HTTP/1.1 200", 12) == 0;
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
    const uint32_t started = now_ms();
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
        const bool storage = device_config_load_wifi(&credentials) == ESP_OK &&
            settings_load(&settings, &loaded) == ESP_OK && loaded != SETTINGS_LOAD_RECOVERED_CORRUPT;
        /* IDF marks the first empty OTA slot VALID before application startup.
         * Only our committed marker proves the personal bootstrap passed its
         * local checks. Once present, ordinary boots need no router. */
        if (s_needs_confirmation && s_personal && s_ota_valid &&
            device_config_bootstrap_confirmed() == ESP_OK) {
            s_needs_confirmation = false;
            atomic_store(&s_pending, false);
            detach_watchdog();
            release_trial();
        }
        if (!s_needs_confirmation) {
            if (s_boot_gate) {
                if (fresh && app_runtime_end_maintenance() == ESP_OK) s_boot_gate = false;
                else {
                    if (now_ms() - started >= 120000) {
                        firmware_update_note_boot_result(FIRMWARE_RESULT_UNSAFE); break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(250)); continue;
                }
            }
            if (storage) { reconcile(); break; }
            if (now_ms() - started >= 120000) break;
        } else {
            wifi_manager_status_t wifi; wifi_manager_get_status(&wifi);
            firmware_health_t health = {.outputs_safe = !status.enabled && !status.heater_on && !status.fan_on,
                .storage_ready = storage, .runtime_fresh = fresh, .sensor_fresh = sensor_fresh,
                .wifi_ready = wifi.connected, .fatal = status.fault == HEAT_FAULT_OUTPUT_INIT || status.fault == HEAT_FAULT_NVS};
            if (atomic_load(&s_startup_done) && fresh && sensor_fresh && wifi.connected) health.http_ready = http_alive();
            const firmware_trial_action_t action = firmware_trial_evaluate_boot(&health,
                now_ms() - started, true, s_rollback_available);
            if (action == FIRMWARE_TRIAL_CONFIRM) {
                const esp_partition_t *running = esp_ota_get_running_partition();
                esp_ota_img_states_t state;
                esp_err_t err = s_personal ? device_config_confirm_bootstrap() : ESP_OK;
                if (err != ESP_OK) { fail_trial(); break; }
                err = esp_ota_get_state_partition(running, &state);
                if (err != ESP_OK || state == ESP_OTA_IMG_UNDEFINED) err = esp_ota_set_boot_partition(running);
                if (err == ESP_OK) err = esp_ota_mark_app_valid_cancel_rollback();
                if (err != ESP_OK) { fail_trial(); break; }
                atomic_store(&s_pending, false);
                firmware_update_note_boot_result(FIRMWARE_RESULT_INSTALLED);
                reconcile();
                if (app_runtime_end_maintenance() != ESP_OK)
                    firmware_update_note_boot_result(FIRMWARE_RESULT_UNSAFE);
                release_trial();
                break;
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
    vTaskDelete(NULL);
}

esp_err_t firmware_trial_start(void)
{
    if (atomic_exchange(&s_started, true)) return ESP_OK;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t read = running ? esp_ota_get_state_partition(running, &state) : ESP_ERR_NOT_FOUND;
    s_personal = (custom_app_desc.image_flags & FIRMWARE_IMAGE_PERSONAL) != 0;
    s_ota_valid = read == ESP_OK && state == ESP_OTA_IMG_VALID;
    s_needs_confirmation = firmware_boot_needs_confirmation(s_ota_valid, s_personal, false);
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
