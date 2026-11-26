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
#include "settings.h"
#include "temperature_monitor.h"

#define SAMPLE_PERIOD_MS 1000U
#define CONTROL_PERIOD_MS 500U
#define COMMAND_QUEUE_DEPTH 8U
#define ACK_RESPONSE_TIMEOUT_MS 500U
#define TEMPERATURE_TASK_STACK 4096
#define CONTROLLER_TASK_STACK 4096
#define MODE_SAVE_TASK_STACK 3072
#define TEMPERATURE_TASK_PRIORITY 5
#define CONTROLLER_TASK_PRIORITY 6
#define MODE_SAVE_TASK_PRIORITY 3

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
/* Guarded by s_admission_lock. Disable invalidates older queued Enables. */
static unsigned s_enable_epoch;
static unsigned s_pending_commands;
static bool s_relay_initialized;
static bool s_relay_known;
static bool s_relay_on;
static heat_mode_t s_saved_heat_mode;
static bool s_heat_mode_write_failed;
static bool s_heat_mode_save_active;
static const app_runtime_enable_guard_t *s_relay_start_guard;
/* Writers hold s_admission_lock. Printer startup and OTA also read the phase. */
static _Atomic(printer_monitor_phase_t) s_monitor_phase;
static uint64_t s_monitor_permit_until;
/* Guarded by s_admission_lock. Reserves one queue slot during persistence. */
static bool s_settings_save_pending;
static TaskHandle_t s_temperature_task;
static TaskHandle_t s_controller_task;
static _Atomic(TaskHandle_t) s_mode_save_task;
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
    RUNTIME_COMMAND_CALIBRATION,
} runtime_command_kind_t;

typedef enum {
    SETTING_TARGET,
    SETTING_RESTART,
    SETTING_CALIBRATION,
} runtime_setting_t;

typedef struct {
    runtime_command_kind_t kind;
    heat_controller_command_t heat;
    float calibration_offset_c;
    unsigned enable_epoch;
    const app_runtime_enable_guard_t *enable_guard;
    uint32_t enable_revision;
    unsigned ack_id;
    uint64_t ack_deadline_ms;
    bool prepare_setting;
} runtime_command_t;

typedef struct {
    unsigned id;
    esp_err_t result;
    float previous_setting;
    float setting_value;
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

static esp_err_t production_set_relay(bool on, void *ctx)
{
    (void) ctx;
    return board_outputs_set(on);
}

static esp_err_t production_save_target(float target_c, void *ctx)
{
    (void) ctx;
    return settings_set_target(target_c);
}

static esp_err_t production_save_hysteresis(float hysteresis_c, void *ctx)
{
    (void) ctx;
    return settings_set_hysteresis(hysteresis_c);
}

static esp_err_t production_save_calibration(float calibration_offset_c, void *ctx)
{
    (void) ctx;
    return settings_set_calibration(calibration_offset_c);
}

/* Caller holds s_admission_lock. Refresh the queued sample with the offset. */
static bool apply_calibration(float offset_c, uint64_t timestamp_ms)
{
    if (!temperature_monitor_set_calibration(&s_monitor, offset_c)) return false;
    if (s_monitor.has_sample) {
        const temperature_sample_t sample = temperature_monitor_snapshot(&s_monitor, timestamp_ms);
        xQueueOverwrite(s_sample_queue, &sample);
    }
    return true;
}

static void publish_status(const heat_controller_snapshot_t *snapshot,
                           const temperature_sample_t *sample)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.state = snapshot->state;
    s_status.fault = snapshot->fault;
    s_status.enabled = snapshot->enabled;
    s_status.heater_on = s_relay_on;
    s_status.fan_on = s_relay_on;
    s_status.target_c = snapshot->target_c;
    s_status.restart_c = snapshot->restart_c;
    s_status.calibration_offset_c = s_monitor.calibration_offset_c;
    if (sample != NULL) s_status.temperature = *sample;
    s_status.wifi_rssi = s_wifi_rssi;
    memcpy(s_status.ip_addr, s_ip_addr, sizeof(s_status.ip_addr));
    s_status.uptime_ms = now_ms();
    xSemaphoreGive(s_status_lock);
}

/* These helpers run under s_admission_lock. */
static esp_err_t apply_relay(bool on)
{
    if (s_relay_known && s_relay_on == on) return ESP_OK;
    const esp_err_t err = s_ops.set_relay(on, s_ops.ctx);
    s_relay_known = err == ESP_OK;
    if (s_relay_known) {
        /* An early reboot can follow a shutoff we no longer remember. */
        if (!on && (!s_relay_initialized || s_relay_on)) {
            heat_controller_note_relay_off(&s_controller, now_ms());
        }
        s_relay_initialized = true;
        s_relay_on = on;
    } else {
        ESP_LOGE(TAG, "relay write failed: %s", esp_err_to_name(err));
    }
    return err;
}

static bool heat_mode_saved(heat_mode_t mode)
{
    return !s_heat_mode_save_active && s_saved_heat_mode == mode && !s_heat_mode_write_failed;
}

/* The caller holds admission on entry and exit. The active flag serializes
 * writes while temperature control and Disable continue to use admission. */
static esp_err_t save_heat_mode(heat_mode_t mode)
{
    if (heat_mode_saved(mode)) return ESP_OK;
    if (s_heat_mode_save_active) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (s_ops.save_heat_mode) {
        s_heat_mode_save_active = true;
        xSemaphoreGive(s_admission_lock);
        err = s_ops.save_heat_mode(mode);
        xSemaphoreTake(s_admission_lock, portMAX_DELAY);
        s_heat_mode_save_active = false;
    }
    /* Retry after a failed commit even if the requested mode equals the last
     * confirmed value. Flash may have changed before the error was reported. */
    s_heat_mode_write_failed = err != ESP_OK;
    if (err == ESP_OK) s_saved_heat_mode = mode;
    return err;
}

/* Keep admission available to the controller and urgent shutdown while
 * waiting for an earlier write to settle. The caller still holds it on exit. */
static esp_err_t wait_for_heat_mode_save(uint64_t deadline_ms)
{
    while (s_heat_mode_save_active) {
        if (deadline_ms && now_ms() >= deadline_ms) return ESP_ERR_TIMEOUT;
        xSemaphoreGive(s_admission_lock);
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    }
    if (!s_running) return ESP_ERR_INVALID_STATE;
    return deadline_ms && now_ms() >= deadline_ms ? ESP_ERR_TIMEOUT : ESP_OK;
}

static void latch_heat_mode_fault(uint64_t timestamp_ms)
{
    atomic_store(&s_monitor_phase, PRINTER_MONITOR_OFF);
    const heat_controller_command_t fault = {
        .kind = HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
        .fault = HEAT_FAULT_NVS,
    };
    (void) heat_controller_command(&s_controller, &fault, NULL, timestamp_ms);
}

static esp_err_t save_active_heat_mode(heat_mode_t mode)
{
    const esp_err_t err = save_heat_mode(mode);
    if (err != ESP_OK) {
        (void) apply_relay(false);
        if (heat_controller_snapshot(&s_controller).fault == HEAT_FAULT_NONE) {
            latch_heat_mode_fault(now_ms());
        }
    }
    return err;
}

static heat_mode_t shutdown_mode(const heat_controller_snapshot_t *snapshot)
{
    if (snapshot->restart_lockout) return HEAT_MODE_LOCKOUT;
    const heat_fault_t fault = snapshot->fault;
    return fault == HEAT_FAULT_NONE || fault == HEAT_FAULT_OUTPUT_INIT || fault == HEAT_FAULT_NVS
        ? HEAT_MODE_OFF : HEAT_MODE_LOCKOUT;
}

/* The relay status is published before any shutdown persistence runs. */
static esp_err_t save_shutdown_mode(heat_mode_t mode)
{
    const esp_err_t err = save_heat_mode(mode);
    if (err != ESP_OK && heat_controller_snapshot(&s_controller).fault == HEAT_FAULT_NONE) {
        latch_heat_mode_fault(now_ms());
        const heat_controller_snapshot_t snapshot = heat_controller_snapshot(&s_controller);
        publish_status(&snapshot, NULL);
    }
    return err;
}

static bool shutdown_mode_needs_save(const heat_controller_snapshot_t *snapshot)
{
    return !snapshot->enabled && s_relay_known && !s_relay_on && !s_heat_mode_save_active &&
        !heat_mode_saved(shutdown_mode(snapshot));
}

static void mode_save_task(void *arg)
{
    (void) arg;
    for (;;) {
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_admission_lock, portMAX_DELAY);
        if (!s_running) {
            atomic_store(&s_mode_save_task, NULL);
            xSemaphoreGive(s_admission_lock);
            break;
        }
        const heat_controller_snapshot_t snapshot = heat_controller_snapshot(&s_controller);
        if (shutdown_mode_needs_save(&snapshot)) {
            (void) save_shutdown_mode(shutdown_mode(&snapshot));
        }
        xSemaphoreGive(s_admission_lock);
    }
    vTaskDelete(NULL);
}

static void clear_heating_request(uint64_t timestamp_ms)
{
    ++s_enable_epoch;
    atomic_store(&s_monitor_phase, PRINTER_MONITOR_OFF);
    const heat_controller_command_t disable = {.kind = HEAT_COMMAND_DISABLE};
    (void) heat_controller_command(&s_controller, &disable, NULL, timestamp_ms);
}

static esp_err_t disable_heating(uint64_t deadline_ms)
{
    clear_heating_request(now_ms());
    const esp_err_t output_err = apply_relay(false);
    heat_controller_snapshot_t snapshot = heat_controller_snapshot(&s_controller);
    publish_status(&snapshot, NULL);
    if (output_err != ESP_OK || heat_mode_saved(shutdown_mode(&snapshot))) return output_err;

    const esp_err_t ready = wait_for_heat_mode_save(deadline_ms);
    if (ready != ESP_OK) return ready;
    /* A newer Enable can only have proceeded after OFF was saved. */
    snapshot = heat_controller_snapshot(&s_controller);
    if (snapshot.enabled) return ESP_ERR_TIMEOUT;
    return save_shutdown_mode(shutdown_mode(&snapshot));
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

        /* The 400 ms conversion is part of the 1000 ms sampling cycle. The
         * independent controller task keeps responding every 500 ms. */
        const uint64_t elapsed_ms = now_ms() - cycle_start_ms;
        vTaskDelay(pdMS_TO_TICKS(elapsed_ms < SAMPLE_PERIOD_MS
            ? SAMPLE_PERIOD_MS - elapsed_ms : 1U));
    }

    s_temperature_task = NULL;
    vTaskDelete(NULL);
}

/* Caller holds admission. The control task owns the freshness check even
 * when the acquisition task is stalled. */
static bool read_control_sample(temperature_sample_t *sample, uint64_t timestamp_ms)
{
    if (xQueuePeek(s_sample_queue, sample, 0) != pdTRUE) return false;
    if (timestamp_ms - sample->timestamp_ms > TEMPERATURE_SAMPLE_MAX_AGE_MS) {
        sample->valid = false;
        sample->consecutive_valid = 0U;
        sample->fault = HEAT_FAULT_SENSOR_STALE;
    }
    return true;
}

/* Caller holds admission. Keep the fault latched until the OFF mode is durable. */
static esp_err_t acknowledge_fault(const runtime_command_t *command,
                                   temperature_sample_t *sample, uint64_t *timestamp_ms)
{
    if (!s_relay_known || s_relay_on) return ESP_ERR_NOT_ALLOWED;
    for (;;) {
        *timestamp_ms = now_ms();
        const temperature_sample_t *reading = read_control_sample(sample, *timestamp_ms) ? sample : NULL;
        heat_controller_t cleared = s_controller;
        if (!heat_controller_command(&cleared, &command->heat, reading, *timestamp_ms)) {
            return ESP_ERR_NOT_ALLOWED;
        }
        /* Check for a new fault in the state that would follow acknowledgement. */
        heat_controller_step(&cleared, reading, *timestamp_ms);
        if (cleared.snapshot.fault != HEAT_FAULT_NONE) {
            cleared.snapshot.restart_lockout = s_controller.snapshot.restart_lockout;
            s_controller = cleared;
            return ESP_ERR_NOT_ALLOWED;
        }
        if (heat_mode_saved(HEAT_MODE_OFF)) {
            s_controller = cleared;
            return ESP_OK;
        }

        const esp_err_t ready = wait_for_heat_mode_save(command->ack_deadline_ms);
        if (ready != ESP_OK) return ready;
        const esp_err_t saved = save_heat_mode(HEAT_MODE_OFF);
        if (saved != ESP_OK) return saved;
        /* Read the probe again after the save released admission. */
    }
}

/* Caller holds admission. The guard stays locked on success. */
static esp_err_t lock_enable_guard(const app_runtime_enable_guard_t *guard,
                                   uint32_t revision, uint64_t deadline_ms)
{
    if (guard && !guard->lock_if_current(revision)) return ESP_ERR_NOT_ALLOWED;
    if (!deadline_ms || now_ms() < deadline_ms) return ESP_OK;
    if (guard) guard->unlock();
    return ESP_ERR_TIMEOUT;
}

static bool lock_enable_if_current(const runtime_command_t *command)
{
    return command->enable_epoch == s_enable_epoch &&
        lock_enable_guard(command->enable_guard, command->enable_revision, command->ack_deadline_ms) == ESP_OK;
}

static void unlock_enable(const runtime_command_t *command)
{
    if (command->enable_guard) command->enable_guard->unlock();
}

static bool enable_is_current(const runtime_command_t *command)
{
    if (!lock_enable_if_current(command)) return false;
    unlock_enable(command);
    return true;
}

static void controller_task(void *arg)
{
    (void) arg;
    temperature_sample_t sample = {
        .probe_c = 0.0f,
        .raw_c = 0.0f,
        .filtered_c = 0.0f,
        .valid = false,
        .fault = HEAT_FAULT_NONE,
    };
    /* Establish a known-off electrical state before any regulation. */
    xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    heat_controller_snapshot_t previous = heat_controller_snapshot(&s_controller);
    (void) apply_relay(false);
    xSemaphoreGive(s_admission_lock);

    while (s_running) {
        runtime_command_t command = {0};
        const bool have_command =
            xQueueReceive(s_command_queue, &command,
                          pdMS_TO_TICKS(CONTROL_PERIOD_MS)) == pdTRUE;
        const bool ack_request = have_command && command.ack_id != 0;
        const bool fault_ack_request = ack_request && command.kind == RUNTIME_COMMAND_HEAT &&
            command.heat.kind == HEAT_COMMAND_ACK_FAULT;
        bool acknowledged = false;
        esp_err_t command_err = ESP_OK;
        float previous_setting = 0.0f;
        float setting_value = 0.0f;

        xSemaphoreTake(s_admission_lock, portMAX_DELAY);
        const bool relay_requested_off = !heat_controller_snapshot(&s_controller).output_on;
        if (relay_requested_off && !s_relay_known) {
            (void) apply_relay(false);
        }
        uint64_t timestamp_ms = now_ms();
        const uint64_t admitted_at = timestamp_ms;
        bool maintenance = atomic_load(&s_maintenance);
        const bool enable_request = have_command && command.kind == RUNTIME_COMMAND_HEAT &&
            command.heat.kind == HEAT_COMMAND_ENABLE;
        const bool current_enable = !enable_request || (s_relay_known && enable_is_current(&command) &&
            !s_heat_mode_save_active &&
            (heat_mode_saved(HEAT_MODE_OFF) || heat_controller_snapshot(&s_controller).enabled));
        /* A setting already saved by its caller must reach the controller
         * even when maintenance starts during persistence. */
        const bool allowed_in_maintenance = command.heat.kind == HEAT_COMMAND_LATCH_EXTERNAL_FAULT ||
            (!command.prepare_setting && (command.kind == RUNTIME_COMMAND_CALIBRATION ||
                                          command.heat.kind == HEAT_COMMAND_SET_TARGET ||
                                          command.heat.kind == HEAT_COMMAND_SET_RESTART));
        const bool allowed_command = have_command && current_enable &&
            (!command.ack_deadline_ms || timestamp_ms < command.ack_deadline_ms) &&
            (!maintenance || allowed_in_maintenance);
        if (have_command && s_pending_commands) --s_pending_commands;
        if (allowed_command && command.prepare_setting) {
            acknowledged = true;
            const heat_controller_snapshot_t current = heat_controller_snapshot(&s_controller);
            if (command.kind == RUNTIME_COMMAND_CALIBRATION) {
                previous_setting = s_monitor.calibration_offset_c;
            } else if (command.heat.kind == HEAT_COMMAND_SET_RESTART) {
                /* The settings lock keeps the target fixed after this queued check. */
                previous_setting = current.target_c - current.restart_c;
                setting_value = current.target_c - command.heat.restart_c;
                acknowledged = heat_restart_is_valid(current.target_c, command.heat.restart_c);
            } else {
                previous_setting = current.target_c;
            }
        } else if (allowed_command && command.kind == RUNTIME_COMMAND_CALIBRATION) {
            acknowledged = apply_calibration(command.calibration_offset_c, timestamp_ms);
        }
        const bool have_sample = read_control_sample(&sample, timestamp_ms);
        const temperature_sample_t *sample_arg = have_sample ? &sample : NULL;

        /* BOOT leaves for IDLE as soon as the sensor has reported once. */
        if (have_sample && heat_controller_snapshot(&s_controller).state ==
                               HEAT_STATE_BOOT) {
            const heat_controller_command_t settle = {
                .kind = HEAT_COMMAND_DISABLE,
            };
            (void) heat_controller_command(&s_controller, &settle, sample_arg,
                                           timestamp_ms);
        }

        if (maintenance) {
            atomic_store(&s_monitor_phase, PRINTER_MONITOR_OFF);
            const heat_controller_command_t disable = {.kind = HEAT_COMMAND_DISABLE};
            (void) heat_controller_command(&s_controller, &disable, sample_arg, timestamp_ms);
        }
        /* Hold the output while an OFF write is unconfirmed or the permit has expired. */
        const bool monitor_holding = atomic_load(&s_monitor_phase) != PRINTER_MONITOR_OFF &&
            timestamp_ms >= s_monitor_permit_until;
        heat_controller_set_inhibit(&s_controller, (relay_requested_off && !s_relay_known) || monitor_holding);
        const bool starts_enable = allowed_command && enable_request &&
            !heat_controller_snapshot(&s_controller).enabled;
        if (allowed_command && command.kind == RUNTIME_COMMAND_HEAT && !command.prepare_setting && !fault_ack_request) {
            acknowledged = heat_controller_command(&s_controller, &command.heat,
                                                    sample_arg, timestamp_ms);
        }
        if (have_sample) {
            heat_controller_step(&s_controller, &sample, timestamp_ms);
        }

        heat_controller_snapshot_t snapshot = heat_controller_snapshot(&s_controller);
        if (starts_enable && acknowledged && snapshot.enabled && apply_relay(false) == ESP_OK) {
            /* Record the interrupted-heat lockout before the relay can turn on. */
            (void) save_active_heat_mode(HEAT_MODE_MANUAL_PENDING);
            timestamp_ms = now_ms();
            maintenance = atomic_load(&s_maintenance);
            if (maintenance) clear_heating_request(timestamp_ms);
            if (read_control_sample(&sample, timestamp_ms)) {
                /* An Enable accepted before the write still needs a healthy
                 * sample when the relay is first energized. */
                if (!sample.valid || sample.consecutive_valid < 3U) {
                    heat_controller_set_inhibit(&s_controller, true);
                }
                heat_controller_step(&s_controller, &sample, timestamp_ms);
            }
            snapshot = heat_controller_snapshot(&s_controller);
        }

        /* The authorization or deadline can change during the flash write.
         * Recheck both before switching on the relay. */
        const bool verify_enable = starts_enable && acknowledged && snapshot.enabled;
        const bool mode_ready = atomic_load(&s_monitor_phase) != PRINTER_MONITOR_OFF ||
            heat_mode_saved(HEAT_MODE_MANUAL_PENDING);
        const bool enable_cancelled = verify_enable && !lock_enable_if_current(&command);
        const bool running = s_running && !atomic_load(&s_maintenance);
        const bool request_on = snapshot.output_on && mode_ready && !enable_cancelled && running;
        const app_runtime_enable_guard_t *relay_guard = s_relay_start_guard;
        /* An initial cloud Enable already holds the same Disable interlock. */
        const bool check_relay_start = request_on && (!s_relay_known || !s_relay_on) && relay_guard &&
            (!verify_enable || command.enable_guard != relay_guard);
        const bool relay_start_blocked = check_relay_start && !relay_guard->lock_relay_start();
        const esp_err_t relay_err = apply_relay(request_on && !relay_start_blocked);
        if (check_relay_start && !relay_start_blocked) relay_guard->unlock_relay_start();
        if (verify_enable && !enable_cancelled) unlock_enable(&command);
        const bool enable_failed = enable_request && acknowledged && snapshot.enabled &&
            (enable_cancelled || relay_start_blocked || relay_err != ESP_OK || !mode_ready || !running);
        if (enable_failed) {
            /* Clear the rejected request before an output retry can run. */
            clear_heating_request(timestamp_ms);
            (void) apply_relay(false);
            snapshot = heat_controller_snapshot(&s_controller);
            acknowledged = false;
        } else if (relay_start_blocked) {
            snapshot.state = HEAT_STATE_HOLDING;
        }
        if (allowed_command && fault_ack_request) {
            command_err = relay_err == ESP_OK ? acknowledge_fault(&command, &sample, &timestamp_ms) : relay_err;
            acknowledged = command_err == ESP_OK;
            snapshot = heat_controller_snapshot(&s_controller);
        }

        if (!snapshot.enabled) {
            atomic_store(&s_monitor_phase, PRINTER_MONITOR_OFF);
        }

        if (snapshot.state != previous.state || snapshot.fault != previous.fault) {
            diagnostics_record(timestamp_ms, previous.state, snapshot.state, snapshot.fault);
            ESP_LOGI(TAG, "state %d -> %d, fault %d", (int) previous.state,
                     (int) snapshot.state, (int) snapshot.fault);
            previous = snapshot;
        }
        publish_status(&snapshot, &sample);
        if (shutdown_mode_needs_save(&snapshot)) {
            const TaskHandle_t writer = atomic_load(&s_mode_save_task);
            if (writer && !enable_failed) {
                xTaskNotifyGive(writer);
            } else {
                /* Confirm a rejected Enable's rollback before replying. */
                (void) save_shutdown_mode(shutdown_mode(&snapshot));
                snapshot = heat_controller_snapshot(&s_controller);
            }
        }
        if (ack_request) {
            /* Confirm the published controller state and known relay state. */
            if (enable_request) {
                acknowledged = acknowledged && snapshot.enabled && snapshot.fault == HEAT_FAULT_NONE && s_relay_known;
            } else if (acknowledged && !command.prepare_setting && command.kind == RUNTIME_COMMAND_HEAT &&
                command.heat.kind == HEAT_COMMAND_SET_TARGET && command.heat.target_c == 0.0f) {
                command_err = relay_err;
            }
            const bool expired = command.ack_deadline_ms && admitted_at >= command.ack_deadline_ms;
            const bool enable_rollback_unconfirmed = enable_failed &&
                (!s_relay_known || !heat_mode_saved(HEAT_MODE_OFF));
            if (!acknowledged && command_err == ESP_OK) command_err = ESP_ERR_NOT_ALLOWED;
            const runtime_ack_reply_t reply = {
                .id = command.ack_id,
                .result = expired || enable_rollback_unconfirmed ? ESP_ERR_TIMEOUT : command_err,
                .previous_setting = previous_setting,
                .setting_value = setting_value,
            };
            xQueueOverwrite(s_ack_queue, &reply);
        }
        xSemaphoreGive(s_admission_lock);
    }

    xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    (void) apply_relay(false);
    xSemaphoreGive(s_admission_lock);
    s_controller_task = NULL;
    vTaskDelete(NULL);
}

/* Caller holds s_admission_lock. */
static esp_err_t enqueue_command(const runtime_command_t *command)
{
    if (xQueueSend(s_command_queue, command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    ++s_pending_commands;
    return ESP_OK;
}

static esp_err_t send_command(const runtime_command_t *command)
{
    if (s_command_queue == NULL || s_admission_lock == NULL || !s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_admission_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (command->ack_deadline_ms && now_ms() >= command->ack_deadline_ms) {
        xSemaphoreGive(s_admission_lock);
        return ESP_ERR_TIMEOUT;
    }
    const bool blocked = atomic_load(&s_maintenance) &&
        command->kind == RUNTIME_COMMAND_HEAT &&
        (command->heat.kind == HEAT_COMMAND_ENABLE || command->heat.kind == HEAT_COMMAND_SET_TARGET ||
         command->heat.kind == HEAT_COMMAND_SET_RESTART || command->heat.kind == HEAT_COMMAND_ACK_FAULT);
    runtime_command_t queued = *command;
    queued.enable_epoch = s_enable_epoch;
    const bool settings_reserved = s_settings_save_pending &&
        (uxQueueSpacesAvailable(s_command_queue) <= 1U ||
         (command->kind == RUNTIME_COMMAND_HEAT && command->heat.kind == HEAT_COMMAND_ENABLE));
    const esp_err_t err = blocked ? ESP_ERR_NOT_ALLOWED :
        (settings_reserved ? ESP_ERR_TIMEOUT : enqueue_command(&queued));
    xSemaphoreGive(s_admission_lock);
    return err;
}

esp_err_t app_runtime_start_with_ops(const app_runtime_ops_t *ops)
{
    if (ops == NULL || ops->read_sensor == NULL || ops->set_relay == NULL ||
        ops->save_target == NULL || ops->save_calibration == NULL ||
        (unsigned) ops->saved_heat_mode > HEAT_MODE_LOCKOUT) {
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
    float hysteresis_c;
    if (settings_load_hysteresis(&hysteresis_c) != ESP_OK) {
        load_result = SETTINGS_LOAD_RECOVERED_CORRUPT;
    }

    s_ops = *ops;
    temperature_monitor_init(&s_monitor, settings.calibration_offset_c);
    heat_controller_init(&s_controller, &(heat_controller_config_t) {
        .target_c = settings.target_c,
        .hysteresis_c = hysteresis_c,
        .overtemp_c = 82.0f,
        .min_transition_ms = 5000,
    });
    if (ops->saved_heat_mode != HEAT_MODE_OFF) {
        const heat_controller_command_t fault = {
            .kind = HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
            .fault = HEAT_FAULT_RESTART_LOCKOUT,
        };
        (void) heat_controller_command(&s_controller, &fault, NULL, now_ms());
    }

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
    s_saved_heat_mode = ops->saved_heat_mode;
    s_heat_mode_write_failed = false;
    s_heat_mode_save_active = false;
    s_relay_start_guard = NULL;
    atomic_store(&s_monitor_phase, PRINTER_MONITOR_OFF);
    s_monitor_permit_until = 0;
    s_relay_initialized = false;
    s_relay_known = false;
    s_relay_on = false;
    s_pending_commands = 0;
    s_settings_save_pending = false;
    s_enable_epoch = 0;

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status = app_status_initial();
    const heat_controller_snapshot_t initial = heat_controller_snapshot(&s_controller);
    s_status.state = initial.state;
    s_status.fault = initial.fault;
    s_status.target_c = settings.target_c;
    s_status.restart_c = initial.restart_c;
    s_status.calibration_offset_c = settings.calibration_offset_c;
    s_wifi_rssi = 0;
    strncpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr) - 1U);
    s_ip_addr[sizeof(s_ip_addr) - 1U] = '\0';
    memcpy(s_status.ip_addr, s_ip_addr, sizeof(s_status.ip_addr));
    xSemaphoreGive(s_status_lock);

    atomic_store(&s_maintenance, s_boot_maintenance);
    s_boot_maintenance = false;
    s_running = true;
    if (s_ops.save_heat_mode) {
        TaskHandle_t writer;
        if (xTaskCreate(mode_save_task, "heat_mode_save", MODE_SAVE_TASK_STACK, NULL,
                        MODE_SAVE_TASK_PRIORITY, &writer) != pdPASS) {
            s_running = false;
            return ESP_ERR_NO_MEM;
        }
        atomic_store(&s_mode_save_task, writer);
    }
    if (xTaskCreate(controller_task, "heat_ctrl", CONTROLLER_TASK_STACK, NULL,
                    CONTROLLER_TASK_PRIORITY, &s_controller_task) != pdPASS) {
        app_runtime_stop();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(temperature_task, "heat_temp", TEMPERATURE_TASK_STACK, NULL,
                    TEMPERATURE_TASK_PRIORITY, &s_temperature_task) != pdPASS) {
        app_runtime_stop();
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

    heat_mode_t saved_heat_mode = HEAT_MODE_OFF;
    const esp_err_t mode_err = settings_load_heat_mode(&saved_heat_mode);

    const app_runtime_ops_t ops = {
        .read_sensor = production_read_sensor,
        .set_relay = production_set_relay,
        .save_target = production_save_target,
        .save_calibration = production_save_calibration,
        .save_hysteresis = production_save_hysteresis,
        .ctx = NULL,
        .saved_heat_mode = saved_heat_mode,
        .save_heat_mode = settings_save_heat_mode,
    };
    err = app_runtime_start_with_ops(&ops);
    if (err == ESP_OK && !outputs_ready) {
        (void) app_runtime_report_startup_fault(HEAT_FAULT_OUTPUT_INIT);
    } else if (err == ESP_OK && mode_err != ESP_OK) {
        (void) app_runtime_report_startup_fault(HEAT_FAULT_NVS);
    }
    return err;
}

void app_runtime_stop(void)
{
    if (!s_running) {
        return;
    }
    xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    s_running = false;
    const TaskHandle_t writer = atomic_load(&s_mode_save_task);
    if (writer) xTaskNotifyGive(writer);
    xSemaphoreGive(s_admission_lock);
    while (s_temperature_task != NULL || s_controller_task != NULL || atomic_load(&s_mode_save_task)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* Outputs are already off. Drain a save before a later start can replace
     * its operations or reset the reserved queue slot. */
    xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    (void) wait_for_heat_mode_save(0);
    xSemaphoreGive(s_admission_lock);
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

/* The caller holds admission while checking the relay and admitted work. */
static bool maintenance_ready_locked(void)
{
    if (!s_running || !atomic_load(&s_maintenance) || !s_relay_known || s_relay_on ||
        s_pending_commands != 0 || s_settings_save_pending) return false;
    const heat_controller_snapshot_t snapshot = heat_controller_snapshot(&s_controller);
    return !snapshot.enabled && heat_mode_saved(shutdown_mode(&snapshot));
}

bool app_runtime_maintenance_ready(void)
{
    if (!s_admission_lock || xSemaphoreTake(s_admission_lock, 0) != pdTRUE) return false;
    const bool ready = maintenance_ready_locked();
    xSemaphoreGive(s_admission_lock);
    return ready;
}

esp_err_t app_runtime_begin_maintenance(uint32_t timeout_ms)
{
    if (!s_running || !s_admission_lock || timeout_ms == 0) return ESP_ERR_INVALID_STATE;
    /* Inhibit the next control iteration immediately. Even a timed-out
     * request must leave inhibition latched while an admitted save drains. */
    atomic_store(&s_maintenance, true);
    const uint64_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        if (app_runtime_maintenance_ready()) return ESP_OK;
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
    const esp_err_t err = maintenance_ready_locked() ? ESP_OK : ESP_ERR_NOT_ALLOWED;
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

static runtime_ack_reply_t wait_ack(unsigned id, uint64_t deadline)
{
    for (;;) {
        const uint64_t time = now_ms();
        if (time >= deadline) return (runtime_ack_reply_t) {.id = id, .result = ESP_ERR_TIMEOUT};
        const uint64_t remaining_ms = deadline - time;
        TickType_t wait = pdMS_TO_TICKS(remaining_ms);
        if (wait == 0U) wait = 1U;
        runtime_ack_reply_t reply;
        /* A tick-aligned queue wait may end before the actual deadline. */
        if (xQueueReceive(s_ack_queue, &reply, wait) == pdTRUE && reply.id == id) return reply;
    }
}

static esp_err_t persist_setting(float value, runtime_setting_t setting)
{
    switch (setting) {
    case SETTING_TARGET: return s_ops.save_target(value, s_ops.ctx);
    case SETTING_RESTART: return s_ops.save_hysteresis(value, s_ops.ctx);
    case SETTING_CALIBRATION: return s_ops.save_calibration(value, s_ops.ctx);
    }
    return ESP_ERR_INVALID_ARG;
}

static void recover_setting_save(float previous_setting, runtime_setting_t setting)
{
    /* A failed commit may have changed flash. Disable heating while the
     * controller's current setting is written back to storage. */
    xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    const uint64_t timestamp_ms = now_ms();
    if (heat_controller_snapshot(&s_controller).fault == HEAT_FAULT_NONE) {
        latch_heat_mode_fault(timestamp_ms);
    }
    (void) disable_heating(0);
    xSemaphoreGive(s_admission_lock);
    if (persist_setting(previous_setting, setting) != ESP_OK) {
        ESP_LOGE(TAG, "could not restore the previous setting after a failed save");
    }
}

static esp_err_t set_setting(float value, runtime_setting_t setting, uint64_t deadline_ms, unsigned ack_id)
{
    if (!isfinite(value) || (setting == SETTING_TARGET && !heat_target_is_valid(value)) ||
        (setting == SETTING_CALIBRATION && (value < SETTINGS_MIN_CALIBRATION_C ||
                                            value > SETTINGS_MAX_CALIBRATION_C))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_running || (setting == SETTING_RESTART && !s_ops.save_hysteresis)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_settings_lock || xSemaphoreTake(s_settings_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_admission_lock || xSemaphoreTake(s_admission_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_settings_lock);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (deadline_ms && now_ms() >= deadline_ms) err = ESP_ERR_TIMEOUT;
    else if (atomic_load(&s_maintenance)) err = ESP_ERR_NOT_ALLOWED;
    else if (uxQueueSpacesAvailable(s_command_queue) == 0) err = ESP_ERR_TIMEOUT;
    if (err != ESP_OK) {
        xSemaphoreGive(s_admission_lock);
        xSemaphoreGive(s_settings_lock);
        return err;
    }

    s_settings_save_pending = true;
    const runtime_command_t command = {
        .kind = setting == SETTING_CALIBRATION ? RUNTIME_COMMAND_CALIBRATION : RUNTIME_COMMAND_HEAT,
        .heat = {
            .kind = setting == SETTING_RESTART ? HEAT_COMMAND_SET_RESTART : HEAT_COMMAND_SET_TARGET,
            .target_c = value,
            .restart_c = value,
        },
        .calibration_offset_c = value,
        .ack_id = ack_id,
    };
    if (ack_id) {
        runtime_command_t prepare = command;
        prepare.prepare_setting = true;
        prepare.ack_deadline_ms = deadline_ms;
        err = enqueue_command(&prepare);
    }
    xSemaphoreGive(s_admission_lock);

    runtime_ack_reply_t prepared = {0};
    if (err == ESP_OK && ack_id) {
        prepared = wait_ack(ack_id, deadline_ms);
        err = prepared.result;
    }
    if (err == ESP_OK) {
        /* Regulation and urgent Disable keep running on the current settings
         * while flash I/O is in progress. */
        const float persisted = setting == SETTING_RESTART ? prepared.setting_value : value;
        err = persist_setting(persisted, setting);
        if (err != ESP_OK && ack_id) recover_setting_save(prepared.previous_setting, setting);
    }

    xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    if (err == ESP_OK) err = s_running ? enqueue_command(&command) : ESP_ERR_INVALID_STATE;
    s_settings_save_pending = false;
    xSemaphoreGive(s_admission_lock);
    if (err == ESP_OK && ack_id) {
        /* The deadline bounds admission. An admitted, durable setting must
         * still take effect if its write takes longer. */
        err = wait_ack(ack_id, now_ms() + ACK_RESPONSE_TIMEOUT_MS).result;
    }
    xSemaphoreGive(s_settings_lock);
    return err;
}

esp_err_t app_runtime_set_target(float target_c)
{
    return set_setting(target_c, SETTING_TARGET, 0, 0);
}

esp_err_t app_runtime_set_calibration(float calibration_offset_c)
{
    return set_setting(calibration_offset_c, SETTING_CALIBRATION, 0, 0);
}

static esp_err_t lock_admission_until(uint64_t deadline_ms, TickType_t wait_without_deadline)
{
    if (!s_running || !s_admission_lock) return ESP_ERR_INVALID_STATE;
    if (!deadline_ms) {
        if (xSemaphoreTake(s_admission_lock, wait_without_deadline) != pdTRUE) return ESP_ERR_TIMEOUT;
    } else {
        for (;;) {
            const uint64_t now = now_ms();
            if (now >= deadline_ms) return ESP_ERR_TIMEOUT;
            const uint64_t remaining_ms = deadline_ms - now;
            const TickType_t wait = pdMS_TO_TICKS(remaining_ms < 100U ? remaining_ms : 100U);
            if (xSemaphoreTake(s_admission_lock, wait ? wait : 1U) == pdTRUE) break;
            if (!s_running) return ESP_ERR_INVALID_STATE;
        }
    }
    const esp_err_t err = !s_running ? ESP_ERR_INVALID_STATE :
        deadline_ms && now_ms() >= deadline_ms ? ESP_ERR_TIMEOUT : ESP_OK;
    if (err != ESP_OK) xSemaphoreGive(s_admission_lock);
    return err;
}

static esp_err_t disable_before(uint64_t deadline_ms)
{
    const esp_err_t locked = lock_admission_until(deadline_ms, portMAX_DELAY);
    if (locked != ESP_OK) return locked;
    const esp_err_t err = disable_heating(deadline_ms);
    xSemaphoreGive(s_admission_lock);
    return err;
}

esp_err_t app_runtime_set_enabled(bool enabled)
{
    if (!enabled) return disable_before(0);
    const runtime_command_t command = {
        .kind = RUNTIME_COMMAND_HEAT,
        .heat = {
            .kind = HEAT_COMMAND_ENABLE,
        },
    };
    return send_command(&command);
}

void app_runtime_set_relay_start_guard(const app_runtime_enable_guard_t *guard)
{
    if (!s_admission_lock || (guard && (!guard->lock_relay_start || !guard->unlock_relay_start))) return;
    xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    s_relay_start_guard = guard;
    xSemaphoreGive(s_admission_lock);
}

static esp_err_t start_ack(void)
{
    if (!s_running || s_ack_lock == NULL || s_ack_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_ack_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (++s_ack_sequence == 0U) ++s_ack_sequence;
    xQueueReset(s_ack_queue);
    return ESP_OK;
}

static esp_err_t confirm_heat(heat_controller_command_kind_t kind, uint64_t deadline_ms,
                              const app_runtime_enable_guard_t *enable_guard, uint32_t enable_revision)
{
    esp_err_t result = start_ack();
    if (result != ESP_OK) return result;
    uint64_t deadline = now_ms() + ACK_RESPONSE_TIMEOUT_MS;
    if (deadline_ms && deadline_ms < deadline) deadline = deadline_ms;
    const runtime_command_t command = {
        .kind = RUNTIME_COMMAND_HEAT,
        .heat = {.kind = kind},
        .enable_guard = enable_guard,
        .enable_revision = enable_revision,
        .ack_id = s_ack_sequence,
        .ack_deadline_ms = deadline,
    };
    result = send_command(&command);
    if (result == ESP_OK) result = wait_ack(command.ack_id, deadline).result;
    xSemaphoreGive(s_ack_lock);
    return result;
}

esp_err_t app_runtime_ack_fault(void)
{
    return app_runtime_ack_fault_until(0);
}

esp_err_t app_runtime_ack_fault_until(uint64_t deadline_ms)
{
    return confirm_heat(HEAT_COMMAND_ACK_FAULT, deadline_ms, NULL, 0);
}

esp_err_t app_runtime_set_enabled_until(bool enabled, uint64_t deadline_ms)
{
    return enabled ? app_runtime_enable_if_current(deadline_ms, NULL, 0) : disable_before(deadline_ms);
}

esp_err_t app_runtime_enable_if_current(uint64_t deadline_ms,
                                        const app_runtime_enable_guard_t *guard, uint32_t revision)
{
    return confirm_heat(HEAT_COMMAND_ENABLE, deadline_ms, guard, revision);
}

static esp_err_t confirm_setting(float value, runtime_setting_t setting, uint64_t deadline_ms)
{
    esp_err_t result = start_ack();
    if (result != ESP_OK) return result;
    const uint64_t timeout = now_ms() + ACK_RESPONSE_TIMEOUT_MS;
    const uint64_t deadline = deadline_ms && deadline_ms < timeout ? deadline_ms : timeout;
    result = set_setting(value, setting, deadline, s_ack_sequence);
    xSemaphoreGive(s_ack_lock);
    return result;
}

esp_err_t app_runtime_set_target_until(float target_c, uint64_t deadline_ms)
{
    return confirm_setting(target_c, SETTING_TARGET, deadline_ms);
}

esp_err_t app_runtime_set_restart_until(float restart_c, uint64_t deadline_ms)
{
    return confirm_setting(restart_c, SETTING_RESTART, deadline_ms);
}

esp_err_t app_runtime_set_calibration_until(float offset_c, uint64_t deadline_ms)
{
    return confirm_setting(offset_c, SETTING_CALIBRATION, deadline_ms);
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

static bool valid_monitor_phase(printer_monitor_phase_t phase)
{
    return phase == PRINTER_MONITOR_WAITING || phase == PRINTER_MONITOR_WATCHING;
}

/* Caller holds admission. Apply holds before starting any flash write. */
static void apply_monitor_permit(uint64_t permit_until_ms)
{
    s_monitor_permit_until = permit_until_ms;
    const uint64_t timestamp_ms = now_ms();
    if (permit_until_ms <= timestamp_ms) {
        heat_controller_set_inhibit(&s_controller, true);
        (void) apply_relay(false);
    }
}

/* Caller holds admission, has validated the permit, and has no write in flight. */
static esp_err_t set_monitor_permit(printer_monitor_phase_t phase,
                                   uint64_t permit_until_ms)
{
    const heat_mode_t mode = phase == PRINTER_MONITOR_WAITING
        ? HEAT_MODE_MONITOR_WAITING : HEAT_MODE_MONITOR_WATCHING;
    const bool starting = atomic_load(&s_monitor_phase) == PRINTER_MONITOR_OFF;
    if (starting) atomic_store(&s_monitor_phase, phase);
    /* A phase change can shorten a live permit immediately. Extending it
     * waits for the updated phase to be durable. */
    const uint64_t interim_permit = !starting && !heat_mode_saved(mode) &&
        s_monitor_permit_until < permit_until_ms ? s_monitor_permit_until : permit_until_ms;
    apply_monitor_permit(interim_permit);
    const esp_err_t err = save_active_heat_mode(mode);
    if (err != ESP_OK) return err;
    if (!s_running || atomic_load(&s_maintenance) ||
        atomic_load(&s_monitor_phase) == PRINTER_MONITOR_OFF) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_monitor_phase, phase);
    apply_monitor_permit(permit_until_ms);
    return ESP_OK;
}

esp_err_t app_runtime_monitor_begin_until(printer_monitor_phase_t phase,
                                         uint64_t permit_until_ms, uint64_t deadline_ms)
{
    const uint64_t lock_deadline = now_ms() + 100U;
    if (!deadline_ms || deadline_ms > lock_deadline) deadline_ms = lock_deadline;
    const esp_err_t locked = lock_admission_until(deadline_ms, pdMS_TO_TICKS(100));
    if (locked != ESP_OK) return locked;
    esp_err_t err = wait_for_heat_mode_save(deadline_ms);
    if (err == ESP_OK) {
        const heat_controller_snapshot_t heat = heat_controller_snapshot(&s_controller);
        if (!valid_monitor_phase(phase) ||
            permit_until_ms > now_ms() + APP_RUNTIME_MONITOR_PERMIT_MAX_MS) {
            err = ESP_ERR_INVALID_ARG;
        } else if (atomic_load(&s_maintenance) ||
                   atomic_load(&s_monitor_phase) != PRINTER_MONITOR_OFF || !heat.enabled ||
                   heat.state == HEAT_STATE_FAULT) {
            err = ESP_ERR_NOT_ALLOWED;
        } else {
            err = set_monitor_permit(phase, permit_until_ms);
        }
    }
    xSemaphoreGive(s_admission_lock);
    return err;
}

esp_err_t app_runtime_monitor_begin(printer_monitor_phase_t phase, uint64_t permit_until_ms)
{
    return app_runtime_monitor_begin_until(phase, permit_until_ms, 0);
}

esp_err_t app_runtime_monitor_permit(printer_monitor_phase_t phase,
                                     uint64_t permit_until_ms)
{
    const uint64_t deadline_ms = now_ms() + 100U;
    const esp_err_t locked = lock_admission_until(deadline_ms, pdMS_TO_TICKS(100));
    if (locked != ESP_OK) return locked;
    esp_err_t err = wait_for_heat_mode_save(deadline_ms);
    if (err == ESP_OK) {
        if (atomic_load(&s_monitor_phase) == PRINTER_MONITOR_OFF ||
            atomic_load(&s_maintenance)) {
            err = ESP_ERR_INVALID_STATE;
        } else if (!valid_monitor_phase(phase) ||
                   permit_until_ms > now_ms() + APP_RUNTIME_MONITOR_PERMIT_MAX_MS) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = set_monitor_permit(phase, permit_until_ms);
        }
    }
    xSemaphoreGive(s_admission_lock);
    return err;
}

esp_err_t app_runtime_monitor_suspend_recovery(void)
{
    if (!s_running || !s_admission_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_admission_lock, portMAX_DELAY);
    esp_err_t err = wait_for_heat_mode_save(0);
    if (err == ESP_OK && (atomic_load(&s_monitor_phase) != PRINTER_MONITOR_OFF || s_heat_mode_write_failed)) {
        /* Invalidate the saved printer phase while retaining the lockout marker. */
        err = save_active_heat_mode(HEAT_MODE_MANUAL_PENDING);
    }
    xSemaphoreGive(s_admission_lock);
    return err;
}

esp_err_t app_runtime_monitor_end_until(bool disable, uint64_t deadline_ms,
                                       const app_runtime_enable_guard_t *guard, uint32_t revision)
{
    const esp_err_t locked = lock_admission_until(deadline_ms, portMAX_DELAY);
    if (locked != ESP_OK) return locked;
    esp_err_t err = ESP_OK;
    if (!disable && !atomic_load(&s_maintenance) &&
        atomic_load(&s_monitor_phase) != PRINTER_MONITOR_OFF) {
        err = wait_for_heat_mode_save(deadline_ms);
    }
    if (err == ESP_OK && atomic_load(&s_monitor_phase) != PRINTER_MONITOR_OFF) {
        if (disable || atomic_load(&s_maintenance)) {
            err = disable_heating(deadline_ms);
        } else if ((err = lock_enable_guard(guard, revision, deadline_ms)) == ESP_OK) {
            if (guard) guard->unlock();
            const heat_mode_t previous_mode = s_saved_heat_mode;
            err = save_active_heat_mode(HEAT_MODE_MANUAL_PENDING);
            if (err == ESP_OK) {
                if (!s_running || atomic_load(&s_maintenance) ||
                    atomic_load(&s_monitor_phase) == PRINTER_MONITOR_OFF) {
                    err = ESP_ERR_NOT_ALLOWED;
                } else {
                    err = lock_enable_guard(guard, revision, deadline_ms);
                    if (err == ESP_OK) {
                        atomic_store(&s_monitor_phase, PRINTER_MONITOR_OFF);
                        if (guard) guard->unlock();
                    } else {
                        /* The monitor still owns the heat. Restore its saved mode. */
                        const esp_err_t restored = save_active_heat_mode(previous_mode);
                        if (restored != ESP_OK) err = restored;
                    }
                }
            }
        }
    }
    xSemaphoreGive(s_admission_lock);
    return err;
}

esp_err_t app_runtime_monitor_end(bool disable)
{
    return app_runtime_monitor_end_until(disable, 0, NULL, 0);
}

printer_monitor_phase_t app_runtime_monitor_phase(void)
{
    return s_running ? atomic_load(&s_monitor_phase) : PRINTER_MONITOR_OFF;
}

bool app_runtime_monitor_active(void)
{
    return app_runtime_monitor_phase() != PRINTER_MONITOR_OFF;
}
