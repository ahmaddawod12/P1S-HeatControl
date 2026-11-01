#include "firmware_update.h"
#include "firmware_job.h"
#include "firmware_transport.h"
#include "app_runtime.h"
#include "device_config.h"
#include "service_operation.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static atomic_bool s_busy;
static atomic_int s_boot_result;
static firmware_status_t s_status;
static firmware_staged_t s_staged;

static esp_err_t check(const char *v, firmware_candidate_t *out, firmware_check_result_t *r, void *ctx)
{ (void) ctx; return firmware_transport_check(v, out, r); }
static esp_err_t begin_maintenance(uint32_t t, void *ctx)
{ (void) ctx; return app_runtime_begin_maintenance(t); }
static esp_err_t end_maintenance(void *ctx)
{ (void) ctx; return app_runtime_end_maintenance(); }
static esp_err_t stage(const firmware_candidate_t *candidate, firmware_progress_cb_t cb, void *c, void *ctx)
{ (void) ctx; return firmware_transport_stage(candidate, cb, c, &s_staged); }
static esp_err_t load(firmware_attempt_t *out, void *ctx)
{ (void) ctx; return device_config_load_attempt(out); }
static esp_err_t save(const firmware_attempt_t *attempt, void *ctx)
{ (void) ctx; return device_config_save_attempt(attempt); }
static esp_err_t activate(void *ctx) { (void) ctx; return firmware_transport_activate(&s_staged); }
static void restart(void *ctx) { (void) ctx; vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); }
static void publish(const firmware_status_t *status, void *ctx)
{
    (void) ctx; xSemaphoreTake(s_lock, portMAX_DELAY); s_status = *status; xSemaphoreGive(s_lock);
}
void firmware_update_note_boot_result(firmware_result_t result) { atomic_store(&s_boot_result, result); }

static void worker(void *arg)
{
    (void) arg;
    const firmware_job_ops_t ops = {.check = check, .begin_maintenance = begin_maintenance,
        .end_maintenance = end_maintenance, .stage = stage, .load_attempt = load,
        .save_attempt = save, .activate = activate, .restart = restart};
    while (true) {
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        firmware_status_t status = {0};
        strcpy(status.installed, custom_app_desc.version);
        firmware_job_run(&status, &ops, publish, NULL);
        atomic_store(&s_busy, false);
        service_operation_end(SERVICE_OPERATION_OTA);
    }
}

esp_err_t firmware_update_start(void)
{
    if (s_task) return ESP_OK;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    strcpy(s_status.installed, custom_app_desc.version);
    if (xTaskCreate(worker, "firmware_ota", 10240, NULL, 3, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
esp_err_t firmware_update_request(void)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    if (firmware_trial_pending() || app_runtime_maintenance_active()) return ESP_ERR_NOT_ALLOWED;
    if (!service_operation_begin(SERVICE_OPERATION_OTA)) return ESP_ERR_NOT_ALLOWED;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        service_operation_end(SERVICE_OPERATION_OTA);
        return ESP_ERR_NOT_ALLOWED;
    }
    atomic_store(&s_boot_result, FIRMWARE_RESULT_NONE);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.phase = FIRMWARE_CHECKING; s_status.result = FIRMWARE_RESULT_NONE;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}
void firmware_update_get_status(firmware_status_t *out)
{
    if (!out) return;
    if (s_lock) { xSemaphoreTake(s_lock, portMAX_DELAY); *out = s_status; xSemaphoreGive(s_lock); }
    else memset(out, 0, sizeof(*out));
    strcpy(out->installed, custom_app_desc.version);
    out->maintenance = app_runtime_maintenance_active();
    out->trial_pending = firmware_trial_pending();
    if (!atomic_load(&s_busy) && atomic_load(&s_boot_result) != FIRMWARE_RESULT_NONE) {
        out->result = (firmware_result_t) atomic_load(&s_boot_result);
        out->phase = out->result == FIRMWARE_RESULT_INSTALLED ? FIRMWARE_SUCCEEDED : FIRMWARE_FAILED;
    }
    else if (out->trial_pending) out->phase = FIRMWARE_TRIAL;
}
