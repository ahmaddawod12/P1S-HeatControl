#include "app_runtime.h"

#include <math.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>

#include "board_outputs.h"
#include "board_pins.h"
#include "diagnostics.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heat_controller.h"
#include "runtime_output_policy.h"
#include "runtime_auto_lease.h"
#include "settings.h"
#include "temperature_monitor.h"

#define SAMPLE_PERIOD_MS 500U
#define CONTROL_PERIOD_MS 100U
#define COMMAND_QUEUE_DEPTH 8U
#define ACK_RESPONSE_TIMEOUT_MS 500U
#define TEMPERATURE_TASK_STACK 4096
#define CONTROLLER_TASK_STACK 4096
#define TEMPERATURE_TASK_PRIORITY 5
#define CONTROLLER_TASK_PRIORITY 6

static const char *TAG = "runtime";

static app_runtime_ops_t s_ops;
static QueueHandle_t s_sample_queue;
static QueueHandle_t s_command_queue;
static QueueHandle_t s_ack_queue;
static SemaphoreHandle_t s_status_lock;
static SemaphoreHandle_t s_admission_lock;
static SemaphoreHandle_t s_settings_lock;
static SemaphoreHandle_t s_ack_lock;
/* Guarded by s_ack_lock; retained across runtime restarts. */
static unsigned s_ack_sequence;
static atomic_bool s_maintenance;
static bool s_boot_maintenance;
static atomic_bool s_maintenance_safe;
static atomic_bool s_urgent_disable;
static atomic_uint s_enable_epoch;
static runtime_auto_lease_t s_auto;
static bool s_auto_enable_pending;
static unsigned s_pending_commands;
/* Guarded by s_admission_lock. Reserves one queue slot during persistence. */
static bool s_settings_save_pending;
static TaskHandle_t s_temperature_task;
static TaskHandle_t s_controller_task;
static volatile bool s_running;

static heat_controller_t s_controller;
static temperature_monitor_t s_monitor;
static temperature_sensor_t s_device;

/* Guarded by s_status_lock. */
static app_status_t s_status;
static int8_t s_wifi_rssi;
static char s_ip_addr[sizeof(s_status.ip_addr)];

typedef enum {
    RUNTIME_COMMAND_HEAT = 0,
    RUNTIME_COMMAND_MANUAL_FAN,
    RUNTIME_COMMAND_CALIBRATION,
} runtime_command_kind_t;

typedef struct {
    runtime_command_kind_t kind;
    heat_controller_command_t heat;
    bool manual_fan_on;
    float calibration_offset_c;
    unsigned enable_epoch;
    unsigned ack_id;
    uint64_t ack_deadline_ms;
} runtime_command_t;

typedef struct {
    unsigned id;
    esp_err_t result;
} runtime_ack_reply_t;

static uint64_t now_ms(void)
{
    return (uint64_t) (esp_timer_get_time() / 1000);
}

static esp_err_t production_read_sensor(temperature_sensor_reading_t *out_reading,
                                        void *ctx)
{
    (void) ctx;
    return temperature_sensor_read(&s_device, out_reading);
}

static esp_err_t production_set_outputs(bool heater_on, bool fan_on, void *ctx)
{
    (void) ctx;
    return board_outputs_set(heater_on, fan_on);
}

static esp_err_t production_save_target(float target_c, void *ctx)
{
    (void) ctx;
    return settings_set_target(target_c);
}

static esp_err_t production_save_calibration(float calibration_offset_c, void *ctx)
{
    (void) ctx;
    return settings_set_calibration(calibration_offset_c);
}

static void publish_status(const heat_controller_snapshot_t *snapshot,
                           const temperature_sample_t *sample,
                           bool heater_on,
                           bool fan_on,
                           bool manual_fan_on)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.state = snapshot->state;
    s_status.fault = snapshot->fault;
    s_status.enabled = snapshot->enabled;
    s_status.heater_on = heater_on;
    s_status.fan_on = fan_on;
    s_status.manual_fan_on = manual_fan_on;
    s_status.target_c = snapshot->target_c;
    s_status.calibration_offset_c = s_monitor.calibration_offset_c;
    s_status.temperature = *sample;
    s_status.wifi_rssi = s_wifi_rssi;
    memcpy(s_status.ip_addr, s_ip_addr, sizeof(s_status.ip_addr));
    s_status.uptime_ms = now_ms();
    xSemaphoreGive(s_status_lock);
}

static void temperature_task(void *arg)
{
    (void) arg;
    while (s_running) {
        const uint64_t cycle_start_ms = now_ms();
        temperature_sensor_reading_t reading = {
            .temperature_c = NAN,
            .fault = TEMPERATURE_SENSOR_FAULT_IO,
        };
        const esp_err_t err = s_ops.read_sensor(&reading, s_ops.ctx);
        const uint64_t timestamp_ms = now_ms();
        if (err != ESP_OK && reading.fault == TEMPERATURE_SENSOR_FAULT_NONE) {
            reading.fault = TEMPERATURE_SENSOR_FAULT_IO;
        }
        xSemaphoreTake(s_admission_lock, portMAX_DELAY);
        temperature_monitor_push(&s_monitor, reading.temperature_c,
                                 reading.fault, timestamp_ms);

        const temperature_sample_t sample =
            temperature_monitor_snapshot(&s_monitor, timestamp_ms);
        xQueueOverwrite(s_sample_queue, &sample);
        xSemaphoreGive(s_admission_lock);

        /* The 400 ms conversion is part of the 500 ms sampling cycle. The
         * independent controller task keeps responding every 100 ms. */
        const uint64_t elapsed_ms = now_ms() - cycle_start_ms;
        vTaskDelay(pdMS_TO_TICKS(elapsed_ms < SAMPLE_PERIOD_MS
            ? SAMPLE_PERIOD_MS - elapsed_ms : 1U));
    }

    s_temperature_task = NULL;
    vTaskDelete(NULL);
}

static void controller_task(void *arg)
{
    (void) arg;
    temperature_sample_t sample = {
        .raw_c = 0.0f,
        .filtered_c = 0.0f,
        .valid = false,
        .fault = HEAT_FAULT_NONE,
    };
    bool have_sample = false;
    bool heater_on = false;
    bool fan_on = false;
    bool manual_fan_on = false;
    heat_controller_snapshot_t previous = heat_controller_snapshot(&s_controller);
    runtime_output_policy_t output_policy;
    runtime_output_policy_init(&output_policy);

    /* Establish a known-off electrical state before any regulation. */
    bool outputs_known = s_ops.set_outputs(false, false, s_ops.ctx) == ESP_OK;

    while (s_running) {
        runtime_command_t command = {0};
        const bool have_command =
            xQueueReceive(s_command_queue, &command,
                          pdMS_TO_TICKS(CONTROL_PERIOD_MS)) == pdTRUE;
        const bool ack_request = have_command && command.kind == RUNTIME_COMMAND_HEAT &&
            command.heat.kind == HEAT_COMMAND_ACK_FAULT;
        bool acknowledged = false;

        xSemaphoreTake(s_admission_lock, portMAX_DELAY);
        const uint64_t timestamp_ms = now_ms();
        if (have_command && s_pending_commands) --s_pending_commands;
        if (have_command && command.kind == RUNTIME_COMMAND_CALIBRATION) {
            (void) temperature_monitor_set_calibration(&s_monitor, command.calibration_offset_c);
            if (s_monitor.has_sample) {
                const temperature_sample_t calibrated = temperature_monitor_snapshot(&s_monitor, timestamp_ms);
                xQueueOverwrite(s_sample_queue, &calibrated);
            }
        }
        if (xQueuePeek(s_sample_queue, &sample, 0) == pdTRUE) {
            have_sample = true;
        }

        if (have_sample && sample.valid &&
            timestamp_ms - sample.timestamp_ms > TEMPERATURE_SAMPLE_MAX_AGE_MS) {
            /* Check age in the control task: acquisition may be stalled and
             * cannot be responsible for timing out its own last sample. */
            sample.valid = false;
            sample.consecutive_valid = 0U;
            sample.fault = HEAT_FAULT_SENSOR_STALE;
        }
        const temperature_sample_t *sample_arg = have_sample ? &sample : NULL;
        const bool maintenance = atomic_load(&s_maintenance);
        runtime_output_policy_set_maintenance(&output_policy, maintenance);

        /* BOOT leaves for IDLE as soon as the sensor has reported once. */
        if (have_sample && heat_controller_snapshot(&s_controller).state ==
                               HEAT_STATE_BOOT) {
            const heat_controller_command_t settle = {
                .kind = HEAT_COMMAND_DISABLE,
            };
            (void) heat_controller_command(&s_controller, &settle, sample_arg,
                                           timestamp_ms);
        }

        const bool auto_was_active = s_auto.status.active;
        (void) runtime_auto_lease_step(&s_auto, timestamp_ms);
        const bool urgent_disable = atomic_exchange(&s_urgent_disable, false);
        if (urgent_disable || maintenance) {
            runtime_auto_lease_revoke(&s_auto, 0, maintenance ? RUNTIME_AUTO_MAINTENANCE : RUNTIME_AUTO_OPERATOR_STOP);
        }
        if (urgent_disable || (auto_was_active && !s_auto.status.active) ||
            (!s_auto.status.active && !s_auto.status.teardown_applied)) {
            const heat_controller_command_t disable = {.kind = HEAT_COMMAND_DISABLE};
            (void) heat_controller_command(&s_controller, &disable, sample_arg, timestamp_ms);
            s_auto_enable_pending = false;
        }
        const bool auto_command_allowed = !have_command || !s_auto.status.active ||
            command.kind == RUNTIME_COMMAND_MANUAL_FAN ||
            (command.kind == RUNTIME_COMMAND_HEAT &&
             (command.heat.kind == HEAT_COMMAND_DISABLE || command.heat.kind == HEAT_COMMAND_LATCH_EXTERNAL_FAULT));
        const bool current_enable = command.kind != RUNTIME_COMMAND_HEAT ||
            command.heat.kind != HEAT_COMMAND_ENABLE || command.enable_epoch == atomic_load(&s_enable_epoch);
        const bool allowed_command = have_command && current_enable && auto_command_allowed &&
            (!ack_request || timestamp_ms < command.ack_deadline_ms) && (!maintenance ||
            command.kind == RUNTIME_COMMAND_CALIBRATION ||
            (command.kind == RUNTIME_COMMAND_HEAT &&
             (command.heat.kind == HEAT_COMMAND_DISABLE ||
              command.heat.kind == HEAT_COMMAND_SET_TARGET ||
              command.heat.kind == HEAT_COMMAND_LATCH_EXTERNAL_FAULT)) ||
            (command.kind == RUNTIME_COMMAND_MANUAL_FAN && !command.manual_fan_on));
        if (have_command && allowed_command) {
            if (command.kind == RUNTIME_COMMAND_MANUAL_FAN) {
                manual_fan_on = command.manual_fan_on;
            } else if (command.kind == RUNTIME_COMMAND_HEAT) {
                const bool applied = heat_controller_command(&s_controller, &command.heat,
                                                              sample_arg, timestamp_ms);
                if (ack_request) acknowledged = applied;
            }
        }
        if (s_auto.status.active && s_auto_enable_pending && !urgent_disable && !maintenance) {
            const heat_controller_command_t enable = {.kind = HEAT_COMMAND_ENABLE};
            if (!heat_controller_command(&s_controller, &enable, sample_arg, timestamp_ms)) {
                runtime_auto_lease_revoke(&s_auto, 0, RUNTIME_AUTO_FAULT);
            }
            s_auto_enable_pending = false;
        }
        if (maintenance) {
            const heat_controller_command_t disable = {.kind = HEAT_COMMAND_DISABLE};
            (void) heat_controller_command(&s_controller, &disable, sample_arg, timestamp_ms);
            manual_fan_on = false;
        }
        if (have_sample) {
            heat_controller_step(&s_controller, &sample, timestamp_ms);
        }

        const heat_controller_snapshot_t snapshot =
            heat_controller_snapshot(&s_controller);
        if (snapshot.state == HEAT_STATE_FAULT) runtime_auto_lease_revoke(&s_auto, 0, RUNTIME_AUTO_FAULT);
        const runtime_output_request_t output_request =
            runtime_output_policy_step(&output_policy, &snapshot, heater_on,
                                       manual_fan_on, timestamp_ms);
        const bool desired_heater_on = output_request.heater_on;
        const bool desired_fan_on = output_request.fan_on;

        if (!outputs_known || desired_heater_on != heater_on || desired_fan_on != fan_on) {
            const esp_err_t err =
                s_ops.set_outputs(desired_heater_on, desired_fan_on, s_ops.ctx);
            if (err == ESP_OK) {
                heater_on = desired_heater_on;
                fan_on = desired_fan_on;
                outputs_known = true;
            } else {
                ESP_LOGE(TAG, "relay write failed: %s", esp_err_to_name(err));
            }
        }

        if (snapshot.state != previous.state || snapshot.fault != previous.fault) {
            diagnostics_record(timestamp_ms, previous.state, snapshot.state, snapshot.fault);
            ESP_LOGI(TAG, "state %d -> %d, fault %d", (int) previous.state,
                     (int) snapshot.state, (int) snapshot.fault);
            previous = snapshot;
        }
        publish_status(&snapshot, &sample, heater_on, fan_on, manual_fan_on);
        if (ack_request) {
            /* A successful command may expose another fault in this control
             * step. Report the final state, after output/status publication. */
            const runtime_ack_reply_t reply = {
                .id = command.ack_id,
                .result = timestamp_ms >= command.ack_deadline_ms ? ESP_ERR_TIMEOUT :
                    acknowledged && snapshot.state == HEAT_STATE_IDLE &&
                    snapshot.fault == HEAT_FAULT_NONE && !snapshot.enabled &&
                    outputs_known && !heater_on ? ESP_OK : ESP_ERR_NOT_ALLOWED,
            };
            xQueueOverwrite(s_ack_queue, &reply);
        }
        if (!s_auto.status.active && !snapshot.enabled && !heater_on && outputs_known) s_auto.status.teardown_applied = true;
        atomic_store(&s_maintenance_safe, maintenance && outputs_known && !heater_on && !fan_on &&
                     s_pending_commands == 0);
        xSemaphoreGive(s_admission_lock);
    }

    (void) s_ops.set_outputs(false, false, s_ops.ctx);
    s_controller_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t send_command(const runtime_command_t *command)
{
    if (s_command_queue == NULL || s_admission_lock == NULL || !s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_admission_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const bool owned = s_auto.status.active && command->kind == RUNTIME_COMMAND_HEAT &&
        (command->heat.kind == HEAT_COMMAND_ENABLE || command->heat.kind == HEAT_COMMAND_SET_TARGET);
    const bool blocked = owned || (atomic_load(&s_maintenance) &&
        ((command->kind == RUNTIME_COMMAND_MANUAL_FAN && command->manual_fan_on) ||
         (command->kind == RUNTIME_COMMAND_HEAT &&
          (command->heat.kind == HEAT_COMMAND_ENABLE || command->heat.kind == HEAT_COMMAND_SET_TARGET ||
           command->heat.kind == HEAT_COMMAND_ACK_FAULT))));
    runtime_command_t queued = *command;
    queued.enable_epoch = atomic_load(&s_enable_epoch);
    const bool settings_reserved = s_settings_save_pending &&
        (uxQueueSpacesAvailable(s_command_queue) <= 1U ||
         (command->kind == RUNTIME_COMMAND_HEAT && command->heat.kind == HEAT_COMMAND_ENABLE));
    const esp_err_t err = blocked ? ESP_ERR_NOT_ALLOWED :
        (!settings_reserved && xQueueSend(s_command_queue, &queued, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT);
    if (err == ESP_OK) ++s_pending_commands;
    xSemaphoreGive(s_admission_lock);
    return err;
}

esp_err_t app_runtime_start_with_ops(const app_runtime_ops_t *ops)
{
    if (ops == NULL || ops->read_sensor == NULL || ops->set_outputs == NULL ||
        ops->save_target == NULL || ops->save_calibration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    settings_data_t settings;
    settings_load_result_t load_result = SETTINGS_LOAD_LOADED;
    esp_err_t err = settings_load(&settings, &load_result);
    if (err != ESP_OK) {
        settings_factory_defaults(&settings);
        load_result = SETTINGS_LOAD_RECOVERED_CORRUPT;
    }

    s_ops = *ops;
    temperature_monitor_init(&s_monitor, settings.calibration_offset_c);
    heat_controller_init(&s_controller, &(heat_controller_config_t) {
        .target_c = settings.target_c,
        .hysteresis_c = 1.0f,
        .overtemp_c = 82.0f,
        .min_transition_ms = 5000,
    });

    if (s_status_lock == NULL) {
        s_status_lock = xSemaphoreCreateMutex();
    }
    if (s_admission_lock == NULL) s_admission_lock = xSemaphoreCreateMutex();
    if (s_settings_lock == NULL) s_settings_lock = xSemaphoreCreateMutex();
    if (s_ack_lock == NULL) s_ack_lock = xSemaphoreCreateMutex();
    if (s_ack_queue == NULL) s_ack_queue = xQueueCreate(1, sizeof(runtime_ack_reply_t));
    if (s_sample_queue == NULL) {
        s_sample_queue = xQueueCreate(1, sizeof(temperature_sample_t));
    }
    if (s_command_queue == NULL) {
        s_command_queue =
            xQueueCreate(COMMAND_QUEUE_DEPTH, sizeof(runtime_command_t));
    }
    if (s_status_lock == NULL || s_admission_lock == NULL || s_settings_lock == NULL ||
        s_ack_lock == NULL || s_ack_queue == NULL || s_sample_queue == NULL || s_command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xQueueReset(s_sample_queue);
    xQueueReset(s_command_queue);
    xQueueReset(s_ack_queue);
    /* Initialize the static diagnostics mutex before HTTP and controller tasks
     * can access the ring concurrently. A restarted runtime starts new history. */
    diagnostics_reset();
    runtime_auto_lease_init(&s_auto);
    s_auto_enable_pending = false;
    s_pending_commands = 0;
    s_settings_save_pending = false;
    atomic_store(&s_urgent_disable, false);
    atomic_store(&s_enable_epoch, 0);

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status = app_status_initial();
    s_status.target_c = settings.target_c;
    s_status.calibration_offset_c = settings.calibration_offset_c;
    s_wifi_rssi = 0;
    strncpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr) - 1U);
    s_ip_addr[sizeof(s_ip_addr) - 1U] = '\0';
    memcpy(s_status.ip_addr, s_ip_addr, sizeof(s_status.ip_addr));
    xSemaphoreGive(s_status_lock);

    atomic_store(&s_maintenance, s_boot_maintenance);
    s_boot_maintenance = false;
    atomic_store(&s_maintenance_safe, false);
    s_running = true;
    if (xTaskCreate(controller_task, "heat_ctrl", CONTROLLER_TASK_STACK, NULL,
                    CONTROLLER_TASK_PRIORITY, &s_controller_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(temperature_task, "heat_temp", TEMPERATURE_TASK_STACK, NULL,
                    TEMPERATURE_TASK_PRIORITY, &s_temperature_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    if (load_result == SETTINGS_LOAD_RECOVERED_CORRUPT) {
        ESP_LOGW(TAG, "settings recovered to defaults; latching NVS fault");
        (void) app_runtime_report_startup_fault(HEAT_FAULT_NVS);
    }
    return ESP_OK;
}

esp_err_t app_runtime_start(void)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = board_outputs_init(NULL);
    const bool outputs_ready = err == ESP_OK;
    if (!outputs_ready) {
        ESP_LOGE(TAG, "relay init failed: %s", esp_err_to_name(err));
    }

    const temperature_sensor_config_t sensor_config =
        temperature_sensor_default_config(BOARD_TEMPERATURE_SENSOR_GPIO);
    err = temperature_sensor_init(&s_device, &sensor_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor init failed: %s", esp_err_to_name(err));
        return err;
    }

    const app_runtime_ops_t ops = {
        .read_sensor = production_read_sensor,
        .set_outputs = production_set_outputs,
        .save_target = production_save_target,
        .save_calibration = production_save_calibration,
        .ctx = NULL,
    };
    err = app_runtime_start_with_ops(&ops);
    if (err == ESP_OK && !outputs_ready) {
        (void) app_runtime_report_startup_fault(HEAT_FAULT_OUTPUT_INIT);
    }
    return err;
}

void app_runtime_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    while (s_temperature_task != NULL || s_controller_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* Outputs are already off. Drain a save before a later start can replace
     * its operations or reset the reserved queue slot. */
    xSemaphoreTake(s_settings_lock, portMAX_DELAY);
    xSemaphoreGive(s_settings_lock);
    xSemaphoreTake(s_ack_lock, portMAX_DELAY);
    xSemaphoreGive(s_ack_lock);
    if (s_device.initialized) {
        (void) temperature_sensor_deinit(&s_device);
    }
    atomic_store(&s_maintenance, false);
}

esp_err_t app_runtime_set_boot_maintenance(bool enabled)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_boot_maintenance = enabled;
    atomic_store(&s_maintenance, enabled);
    return ESP_OK;
}

bool app_runtime_maintenance_active(void) { return atomic_load(&s_maintenance); }

esp_err_t app_runtime_begin_maintenance(uint32_t timeout_ms)
{
    if (!s_running || !s_admission_lock || timeout_ms == 0) return ESP_ERR_INVALID_STATE;
    /* Inhibit the next control iteration immediately. Even a timed-out
     * request must leave inhibition latched while an admitted save drains. */
    atomic_store(&s_maintenance_safe, false);
    atomic_store(&s_maintenance, true);
    const uint64_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        if (atomic_load(&s_maintenance_safe)) return ESP_OK;
        app_status_t status;
        app_runtime_get_status(&status);
        if (now_ms() - start >= 2000 && status.heater_on) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t app_runtime_end_maintenance(void)
{
    if (!s_running || !s_admission_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_admission_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const esp_err_t err = atomic_load(&s_maintenance_safe) ? ESP_OK : ESP_ERR_NOT_ALLOWED;
    if (err == ESP_OK) atomic_store(&s_maintenance, false);
    xSemaphoreGive(s_admission_lock);
    return err;
}

esp_err_t app_runtime_report_startup_fault(heat_fault_t fault)
{
    if (fault != HEAT_FAULT_NVS && fault != HEAT_FAULT_OUTPUT_INIT) {
        return ESP_ERR_INVALID_ARG;
    }
    const runtime_command_t command = {
        .kind = RUNTIME_COMMAND_HEAT,
        .heat = {
            .kind = HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
            .fault = fault,
        },
    };
    return send_command(&command);
}

static esp_err_t set_setting(float value, bool target)
{
    if (target ? !heat_target_is_valid(value) :
        (!isfinite(value) || value < SETTINGS_MIN_CALIBRATION_C || value > SETTINGS_MAX_CALIBRATION_C)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_settings_lock || xSemaphoreTake(s_settings_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_admission_lock || xSemaphoreTake(s_admission_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_settings_lock);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (atomic_load(&s_maintenance) || s_auto.status.active) err = ESP_ERR_NOT_ALLOWED;
    else if (uxQueueSpacesAvailable(s_command_queue) == 0) err = ESP_ERR_TIMEOUT;
    if (err == ESP_OK) {
        s_settings_save_pending = true;
        ++s_pending_commands;
    }
    xSemaphoreGive(s_admission_lock);
    if (err != ESP_OK) {
        xSemaphoreGive(s_settings_lock);
        return err;
    }

    /* Only other settings setters wait for this save. Regulation, faults and
     * urgent Disable keep running under their short admission lock. */
    err = target ? s_ops.save_target(value, s_ops.ctx) : s_ops.save_calibration(value, s_ops.ctx);
    const runtime_command_t command = {
        .kind = target ? RUNTIME_COMMAND_HEAT : RUNTIME_COMMAND_CALIBRATION,
        .heat = {
            .kind = HEAT_COMMAND_SET_TARGET,
            .target_c = value,
        },
        .calibration_offset_c = value,
    };
    xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        /* send_command leaves this slot available until persistence ends. */
        err = xQueueSend(s_command_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    if (err != ESP_OK) --s_pending_commands;
    s_settings_save_pending = false;
    xSemaphoreGive(s_admission_lock);
    xSemaphoreGive(s_settings_lock);
    return err;
}

esp_err_t app_runtime_set_target(float target_c)
{
    return set_setting(target_c, true);
}

esp_err_t app_runtime_set_calibration(float calibration_offset_c)
{
    return set_setting(calibration_offset_c, false);
}

esp_err_t app_runtime_set_enabled(bool enabled)
{
    if (!enabled) {
        if (!s_running) return ESP_ERR_INVALID_STATE;
        atomic_fetch_add(&s_enable_epoch, 1);
        atomic_store(&s_urgent_disable, true);
        return ESP_OK;
    }
    const runtime_command_t command = {
        .kind = RUNTIME_COMMAND_HEAT,
        .heat = {
            .kind = enabled ? HEAT_COMMAND_ENABLE : HEAT_COMMAND_DISABLE,
        },
    };
    return send_command(&command);
}

esp_err_t app_runtime_set_manual_fan(bool enabled)
{
    const runtime_command_t command = {
        .kind = RUNTIME_COMMAND_MANUAL_FAN,
        .manual_fan_on = enabled,
    };
    return send_command(&command);
}

esp_err_t app_runtime_ack_fault(void)
{
    if (!s_running || s_ack_lock == NULL || s_ack_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_ack_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (++s_ack_sequence == 0U) ++s_ack_sequence;
    xQueueReset(s_ack_queue);
    const uint64_t deadline = now_ms() + ACK_RESPONSE_TIMEOUT_MS;
    const runtime_command_t command = {
        .kind = RUNTIME_COMMAND_HEAT,
        .heat = {
            .kind = HEAT_COMMAND_ACK_FAULT,
        },
        .ack_id = s_ack_sequence,
        .ack_deadline_ms = deadline,
    };
    esp_err_t result = send_command(&command);
    if (result == ESP_OK) {
        result = ESP_ERR_TIMEOUT;
        while (true) {
            const uint64_t time = now_ms();
            if (time >= deadline) break;
            const uint64_t remaining_ms = deadline - time;
            TickType_t wait = pdMS_TO_TICKS(remaining_ms);
            if (wait == 0U) wait = 1U;
            runtime_ack_reply_t reply;
            /* Tick-aligned queue waits can end before the monotonic deadline.
             * Keep waiting so an eligible queued request never outlives failure. */
            if (xQueueReceive(s_ack_queue, &reply, wait) != pdTRUE) continue;
            if (reply.id == command.ack_id) { result = reply.result; break; }
        }
    }
    xSemaphoreGive(s_ack_lock);
    return result;
}

void app_runtime_set_network_status(int8_t rssi, const char *ip_addr)
{
    if (s_status_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_wifi_rssi = rssi;
    if (ip_addr != NULL) {
        strncpy(s_ip_addr, ip_addr, sizeof(s_ip_addr) - 1U);
        s_ip_addr[sizeof(s_ip_addr) - 1U] = '\0';
    }
    s_status.wifi_rssi = s_wifi_rssi;
    memcpy(s_status.ip_addr, s_ip_addr, sizeof(s_status.ip_addr));
    xSemaphoreGive(s_status_lock);
}

void app_runtime_get_status(app_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    if (s_status_lock == NULL) {
        *out_status = app_status_initial();
        return;
    }
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *out_status = s_status;
    xSemaphoreGive(s_status_lock);
}

esp_err_t app_runtime_auto_acquire(uint64_t id,float target,uint64_t until)
{
    if (!s_running || !s_admission_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_admission_lock,pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    app_status_t status; app_runtime_get_status(&status);
    const uint64_t now=now_ms();
    esp_err_t err=ESP_ERR_NOT_ALLOWED;
    if (!atomic_load(&s_maintenance) && !atomic_load(&s_urgent_disable) && !s_pending_commands &&
        !status.enabled && status.fault==HEAT_FAULT_NONE && status.temperature.valid &&
        status.temperature.consecutive_valid>=3 && now>=status.temperature.timestamp_ms &&
        now-status.temperature.timestamp_ms<=TEMPERATURE_SAMPLE_MAX_AGE_MS &&
        target>0 && heat_target_is_valid(target) && target==status.target_c &&
        status.temperature.raw_c<82 && status.temperature.filtered_c<82 && s_auto.status.teardown_applied) {
        err=runtime_auto_lease_acquire(&s_auto,id,now,until);
        if (err==ESP_OK) s_auto_enable_pending=true;
    }
    xSemaphoreGive(s_admission_lock); return err;
}
esp_err_t app_runtime_auto_renew(uint64_t id,uint64_t until)
{
    if (!s_running || !s_admission_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_admission_lock,pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err=runtime_auto_lease_renew(&s_auto,id,now_ms(),until);
    xSemaphoreGive(s_admission_lock); return err;
}
void app_runtime_auto_revoke(uint64_t id)
{
    if (!id) { atomic_fetch_add(&s_enable_epoch,1); atomic_store(&s_urgent_disable,true); return; }
    if (!s_running || !s_admission_lock) return;
    if (xSemaphoreTake(s_admission_lock,pdMS_TO_TICKS(100)) != pdTRUE) return;
    runtime_auto_lease_revoke(&s_auto,id,RUNTIME_AUTO_OPERATOR_STOP);
    xSemaphoreGive(s_admission_lock);
}
void app_runtime_get_auto_status(runtime_auto_status_t *out)
{
    if (!out) return;
    *out=(runtime_auto_status_t){0};
    if (!s_running || !s_admission_lock) return;
    if (xSemaphoreTake(s_admission_lock,pdMS_TO_TICKS(100)) != pdTRUE) return;
    *out=s_auto.status;xSemaphoreGive(s_admission_lock);
}
