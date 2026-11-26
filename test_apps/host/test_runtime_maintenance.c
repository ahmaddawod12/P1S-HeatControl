/* Run the real controller and temperature tasks with threaded RTOS boundaries.
 * No relay hardware or NVS is touched by these tests. */
#include "app_runtime.h"
#include "board_outputs.h"
#include "settings.h"
#include "esp_timer.h"
#include "diagnostics.h"
#include "host_freertos.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool heater, fail_output, fail_output_on, block_save;
static atomic_uint relay_activations;
static uint32_t remote_revision;
static bool disable_pending;
static atomic_uint blocked_relay_starts;
static _Atomic(heat_mode_t) saved_heat_mode;
static atomic_uint heat_mode_saves;
static atomic_bool fail_next_heat_mode_save;
static atomic_uint saves;
static _Atomic float saved_target = 60.0f;
static atomic_bool fail_next_target_save_after_write;
static atomic_int sensor_fault;
static _Atomic float sensor_temperature_c = 25.0f;
static atomic_int save_result = ESP_OK;
static atomic_bool slow_sensor, late_sample, early_sample;
static atomic_uint slow_reads;
static atomic_ullong last_slow_read_ms;
static atomic_bool stall_sensor, stall_entered, release_stall;
static SemaphoreHandle_t save_entered, save_release, setter_done;
static SemaphoreHandle_t mode_save_entered, mode_save_release;
static SemaphoreHandle_t remote_enable_lock;
static heat_mode_t blocked_mode = HEAT_MODE_MONITOR_WAITING;
static bool block_after_mode_write;
static esp_err_t setter_result;
static _Atomic float saved_calibration;
static _Atomic float saved_hysteresis = 1.0f;
static atomic_uint calibration_saves;
static uint64_t runtime_started_ms;

void settings_factory_defaults(settings_data_t *data)
{ *data = (settings_data_t) {SETTINGS_SCHEMA_VERSION, 60.0f, 0.0f}; }
esp_err_t settings_load(settings_data_t *data, settings_load_result_t *result)
{ settings_factory_defaults(data); data->calibration_offset_c = atomic_load(&saved_calibration);
  *result = SETTINGS_LOAD_LOADED; return ESP_OK; }
esp_err_t settings_load_heat_mode(heat_mode_t *mode)
{ *mode = atomic_load(&saved_heat_mode); return ESP_OK; }
esp_err_t settings_save_heat_mode(heat_mode_t mode)
{
    if (mode == blocked_mode && mode_save_entered) {
        if (block_after_mode_write) atomic_store(&saved_heat_mode, mode);
        xSemaphoreGive(mode_save_entered);
        assert(xSemaphoreTake(mode_save_release, portMAX_DELAY) == pdTRUE);
    }
    atomic_fetch_add(&heat_mode_saves, 1);
    atomic_store(&saved_heat_mode, mode);
    return atomic_exchange(&fail_next_heat_mode_save, false) ? ESP_FAIL : ESP_OK;
}
esp_err_t settings_set_target(float value) { (void) value; return ESP_OK; }
esp_err_t settings_load_hysteresis(float *value)
{ *value = atomic_load(&saved_hysteresis); return ESP_OK; }
esp_err_t settings_set_hysteresis(float value)
{ atomic_store(&saved_hysteresis, value); return ESP_OK; }
esp_err_t settings_set_calibration(float value)
{
    atomic_fetch_add(&calibration_saves, 1);
    if (atomic_load(&save_result) != ESP_OK) return atomic_load(&save_result);
    atomic_store(&saved_calibration, value);
    return ESP_OK;
}
temperature_sensor_config_t temperature_sensor_default_config(int gpio)
{ return (temperature_sensor_config_t) {gpio}; }
esp_err_t temperature_sensor_init(temperature_sensor_t *device, const temperature_sensor_config_t *config)
{ (void) device; (void) config; return ESP_OK; }
esp_err_t temperature_sensor_deinit(temperature_sensor_t *device)
{ device->initialized = false; return ESP_OK; }
esp_err_t temperature_sensor_read(temperature_sensor_t *device, temperature_sensor_reading_t *reading)
{
    (void) device;
    if (atomic_load(&stall_sensor)) {
        atomic_store(&stall_entered, true);
        while (!atomic_load(&release_stall)) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (atomic_load(&slow_sensor)) {
        const uint64_t started_ms = (uint64_t) esp_timer_get_time() / 1000;
        const uint64_t previous_ms = atomic_exchange(&last_slow_read_ms, started_ms);
        if (previous_ms && started_ms - previous_ms > 1150U) atomic_store(&late_sample, true);
        if (previous_ms && started_ms - previous_ms < 850U) atomic_store(&early_sample, true);
        atomic_fetch_add(&slow_reads, 1);
        vTaskDelay(pdMS_TO_TICKS(400)); /* Real DS18B20 driver conversion wait. */
    }
    *reading = (temperature_sensor_reading_t) {.temperature_c = atomic_load(&sensor_temperature_c),
        .fault = (temperature_sensor_fault_t) atomic_load(&sensor_fault)};
    return reading->fault == TEMPERATURE_SENSOR_FAULT_OPEN ? ESP_ERR_NOT_FOUND : ESP_OK;
}
esp_err_t board_outputs_init(const board_outputs_config_t *config)
{ (void) config; return ESP_OK; }
esp_err_t board_outputs_set(bool on)
{
    if (atomic_load(&fail_output) || (on && atomic_load(&fail_output_on))) return ESP_FAIL;
    if (on && remote_enable_lock) assert(xSemaphoreTake(remote_enable_lock, 0) == pdFALSE);
    if (on) atomic_fetch_add(&relay_activations, 1);
    atomic_store(&heater, on); return ESP_OK;
}
static esp_err_t read_sensor(temperature_sensor_reading_t *out, void *ctx)
{ (void) ctx; return temperature_sensor_read(NULL, out); }
static esp_err_t set_relay(bool on, void *ctx)
{ (void) ctx; return board_outputs_set(on); }
static esp_err_t save_target(float value, void *ctx)
{
    (void) ctx;
    if (atomic_load(&block_save)) {
        xSemaphoreGive(save_entered);
        assert(xSemaphoreTake(save_release, portMAX_DELAY) == pdTRUE);
    }
    atomic_fetch_add(&saves, 1);
    const esp_err_t result = atomic_load(&save_result);
    if (result != ESP_OK) return result;
    atomic_store(&saved_target, value);
    return atomic_exchange(&fail_next_target_save_after_write, false) ? ESP_FAIL : ESP_OK;
}
static esp_err_t save_calibration(float value, void *ctx)
{ (void) ctx; return settings_set_calibration(value); }
static esp_err_t save_hysteresis(float value, void *ctx)
{ (void) ctx; return settings_set_hysteresis(value); }
static uint64_t now_ms(void) { return (uint64_t) esp_timer_get_time() / 1000; }
static app_status_t wait_status(bool ready, bool heat, uint32_t timeout)
{
    const uint64_t start = now_ms();
    app_status_t status;
    do {
        app_runtime_get_status(&status);
        assert(status.heater_on == status.fan_on);
        if ((!ready || status.temperature.consecutive_valid >= 3) && status.heater_on == heat) return status;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (now_ms() - start < timeout);
    fprintf(stderr,"wait wanted heat=%d ready=%d: state=%d enabled=%d heat=%d fault=%d temp=%.2f streak=%u monitor=%d\n",heat,ready,status.state,status.enabled,status.heater_on,status.fault,status.temperature.filtered_c,status.temperature.consecutive_valid,app_runtime_monitor_active());
    assert(!"controller did not reach expected state");
    return status;
}
static void start_with_recovery(heat_mode_t mode, bool persist)
{
    const app_runtime_ops_t ops = {
        .read_sensor = read_sensor,
        .set_relay = set_relay,
        .save_target = save_target,
        .save_calibration = save_calibration,
        .save_hysteresis = save_hysteresis,
        .saved_heat_mode = mode,
        .save_heat_mode = persist ? settings_save_heat_mode : NULL,
    };
    assert(app_runtime_start_with_ops(&ops) == ESP_OK);
    runtime_started_ms = now_ms();
}

static void wait_initial_relay_interval(void)
{
    while (now_ms() - runtime_started_ms < 5200U) vTaskDelay(pdMS_TO_TICKS(20));
}
static void start(void)
{
    start_with_recovery(HEAT_MODE_OFF, false);
    (void) wait_status(true, false, 4000);
}
static void enable(void)
{
    assert(app_runtime_set_enabled(true) == ESP_OK);
    (void) wait_status(true, true, 7000);
    assert(atomic_load(&heater));
}

static void wait_saved_heat_mode(heat_mode_t expected)
{
    const uint64_t deadline = now_ms() + 2000U;
    while (atomic_load(&saved_heat_mode) != expected && now_ms() < deadline) vTaskDelay(1);
    assert(atomic_load(&saved_heat_mode) == expected);
}

static void saved_heat_requires_acknowledgement(heat_mode_t mode)
{
    const unsigned activations = atomic_load(&relay_activations);
    atomic_store(&saved_heat_mode, mode);
    const uint64_t boot_started = now_ms();
    assert(app_runtime_start() == ESP_OK);
    const app_status_t status = wait_status(true, false, 4000);
    assert(status.fault == HEAT_FAULT_RESTART_LOCKOUT && !status.enabled && !status.restore_pending);
    while (now_ms() - boot_started < 5500U) {
        assert(!atomic_load(&heater));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    assert(app_runtime_set_enabled_until(true, now_ms() + 1000U) == ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_set_enabled_until(false, now_ms() + 1500U) == ESP_OK);
    assert(wait_status(true, false, 1000).fault == HEAT_FAULT_RESTART_LOCKOUT);
    atomic_store(&fail_next_heat_mode_save, true);
    assert(app_runtime_ack_fault() == ESP_FAIL);
    assert(wait_status(true, false, 1000).fault == HEAT_FAULT_RESTART_LOCKOUT);
    wait_saved_heat_mode(HEAT_MODE_LOCKOUT);
    assert(app_runtime_ack_fault() == ESP_OK);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_OFF);
    const app_status_t acknowledged = wait_status(true, false, 1000);
    assert(acknowledged.fault == HEAT_FAULT_NONE && !acknowledged.enabled);
    assert(atomic_load(&relay_activations) == activations);
    app_runtime_stop();
}

static void startup_storage_fault_preserves_restart_lockout(void)
{
    atomic_store(&sensor_temperature_c, 81.0f);
    atomic_store(&saved_calibration, -2.0f);
    atomic_store(&saved_heat_mode, HEAT_MODE_LOCKOUT);
    start_with_recovery(HEAT_MODE_LOCKOUT, true);
    (void) wait_status(true, false, 5000);
    assert(app_runtime_report_startup_fault(HEAT_FAULT_NVS) == ESP_OK);
    assert(app_runtime_ack_fault() == ESP_ERR_NOT_ALLOWED);
    assert(wait_status(true, false, 1000).fault == HEAT_FAULT_NVS);
    assert(app_runtime_set_enabled_until(false, now_ms() + 1500U) == ESP_OK);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_LOCKOUT);
    app_runtime_stop();

    start_with_recovery(atomic_load(&saved_heat_mode), true);
    assert(wait_status(true, false, 5000).fault == HEAT_FAULT_RESTART_LOCKOUT);
    assert(app_runtime_ack_fault() == ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_report_startup_fault(HEAT_FAULT_NVS) == ESP_OK);
    atomic_store(&sensor_temperature_c, 79.0f);
    const uint64_t deadline = now_ms() + 5000U;
    app_status_t status;
    do {
        vTaskDelay(pdMS_TO_TICKS(20));
        app_runtime_get_status(&status);
    } while (status.temperature.probe_c != 79.0f && now_ms() < deadline);
    assert(status.temperature.probe_c == 79.0f && status.fault == HEAT_FAULT_NVS);
    assert(app_runtime_ack_fault() == ESP_OK);
    wait_saved_heat_mode(HEAT_MODE_OFF);
    assert(!wait_status(true, false, 1000).enabled);
    app_runtime_stop();
}

static void boot_maintenance_waits_for_shutdown_save(void)
{
    mode_save_entered = xSemaphoreCreateBinary();
    mode_save_release = xSemaphoreCreateBinary();
    assert(mode_save_entered && mode_save_release);
    blocked_mode = HEAT_MODE_LOCKOUT;
    atomic_store(&saved_heat_mode, HEAT_MODE_MANUAL_PENDING);
    assert(app_runtime_set_boot_maintenance(true) == ESP_OK);
    start_with_recovery(HEAT_MODE_MANUAL_PENDING, true);
    assert(xSemaphoreTake(mode_save_entered, pdMS_TO_TICKS(3000)) == pdTRUE);
    assert(!wait_status(true, false, 5000).enabled);
    assert(!app_runtime_maintenance_ready());
    assert(app_runtime_end_maintenance() == ESP_ERR_NOT_ALLOWED);

    xSemaphoreGive(mode_save_release);
    wait_saved_heat_mode(HEAT_MODE_LOCKOUT);
    const uint64_t deadline = now_ms() + 1500U;
    bool ready = app_runtime_maintenance_ready();
    while (!ready && now_ms() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
        ready = app_runtime_maintenance_ready();
    }
    assert(ready);
    assert(app_runtime_end_maintenance() == ESP_OK);
    assert(!app_runtime_maintenance_active());
    app_runtime_stop();
    vSemaphoreDelete(mode_save_entered);
    vSemaphoreDelete(mode_save_release);
}

static void fresh_enable_after_reboot_waits_five_seconds(void)
{
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 4000);
    enable();
    wait_saved_heat_mode(HEAT_MODE_MANUAL_PENDING);
    assert(app_runtime_set_enabled(false) == ESP_OK);
    assert(!atomic_load(&heater));
    app_runtime_stop();

    const uint64_t boot_started = now_ms();
    start_with_recovery(atomic_load(&saved_heat_mode), true);
    assert(!wait_status(true, false, 4000).restore_pending);
    assert(app_runtime_set_enabled_until(true, now_ms() + 1000U) == ESP_OK);
    assert(!atomic_load(&heater));
    (void) wait_status(true, true, 7000);
    assert(now_ms() - boot_started >= 5000U);
    app_runtime_stop();
}

static void urgent_disable_discards_previously_queued_enables(void)
{
    start();
    for(unsigned round=0;round<20;round++) {
        enable();
        for(unsigned i=0;i<32;i++) (void)app_runtime_set_enabled(true);
        if(round&1) {
            assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
            app_runtime_monitor_end(true);
        }
        else assert(app_runtime_set_enabled(false)==ESP_OK);
        (void)wait_status(true,false,2000);
        vTaskDelay(pdMS_TO_TICKS(100));
        assert(!atomic_load(&heater));
    }
    app_runtime_stop();
}
static void check_rejected(void)
{
    const unsigned count = atomic_load(&saves);
    assert(app_runtime_set_enabled(true) == ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_set_target(55.0f) == ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_ack_fault() == ESP_ERR_NOT_ALLOWED);
    assert(atomic_load(&saves) == count);
    assert(app_runtime_set_enabled(false) == ESP_OK);
}
static void blocked_setter(void *arg)
{ (void) arg; setter_result = app_runtime_set_target(55.0f); xSemaphoreGive(setter_done); vTaskDelete(NULL); }

static void begin_blocked_save_using(void (*setter)(void *))
{
    save_entered = xSemaphoreCreateBinary();
    save_release = xSemaphoreCreateBinary();
    setter_done = xSemaphoreCreateBinary();
    atomic_store(&block_save, true);
    assert(xTaskCreate(setter, "save", 4096, NULL, 5, NULL) == pdPASS);
    assert(xSemaphoreTake(save_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
}

static void begin_blocked_save(void)
{
    begin_blocked_save_using(blocked_setter);
}

static void monitored_heating_ends_on_restart(void)
{
    assert(app_runtime_start() == ESP_OK);
    (void) wait_status(true, false, 4000);
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WAITING, now_ms() + 5000U) == ESP_OK);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_MONITOR_WAITING);
    const unsigned writes = atomic_load(&heat_mode_saves);
    assert(app_runtime_monitor_permit(PRINTER_MONITOR_WAITING, now_ms() + 5000U) == ESP_OK);
    assert(atomic_load(&heat_mode_saves) == writes);
    assert(app_runtime_monitor_suspend_recovery() == ESP_OK);
    assert(app_runtime_monitor_phase() == PRINTER_MONITOR_WAITING);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_MANUAL_PENDING);
    assert(app_runtime_monitor_permit(PRINTER_MONITOR_WAITING, now_ms() + 5000U) == ESP_OK);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_MONITOR_WAITING);
    app_runtime_stop();

    assert(app_runtime_start() == ESP_OK);
    assert(!app_runtime_monitor_active());
    assert(app_runtime_monitor_permit(PRINTER_MONITOR_WAITING, now_ms() + 5000U) == ESP_ERR_INVALID_STATE);
    const app_status_t waiting = wait_status(true, false, 4000);
    assert(!waiting.enabled && waiting.fault == HEAT_FAULT_RESTART_LOCKOUT);
    assert(app_runtime_ack_fault() == ESP_OK);
    wait_saved_heat_mode(HEAT_MODE_OFF);

    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_MONITOR_WATCHING);
    (void) wait_status(true, true, 2000);
    app_runtime_stop();

    assert(app_runtime_start() == ESP_OK);
    assert(!app_runtime_monitor_active());
    assert(app_runtime_monitor_permit(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_ERR_INVALID_STATE);
    const app_status_t restored = wait_status(true, false, 4000);
    assert(!restored.enabled && restored.fault == HEAT_FAULT_RESTART_LOCKOUT);
    assert(app_runtime_ack_fault() == ESP_OK);
    wait_saved_heat_mode(HEAT_MODE_OFF);
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
    assert(app_runtime_monitor_end(true) == ESP_OK);
    assert(!atomic_load(&heater));
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_OFF);
    app_runtime_stop();

    assert(app_runtime_start() == ESP_OK);
    assert(!app_runtime_monitor_active());
    assert(!wait_status(true, false, 4000).enabled);
    app_runtime_stop();
}

static void stopping_monitor_after_reboot_keeps_heat_disabled(void)
{
    atomic_store(&saved_heat_mode, HEAT_MODE_MONITOR_WAITING);
    assert(app_runtime_start() == ESP_OK);
    assert(!app_runtime_monitor_active());
    assert(app_runtime_monitor_end(false) == ESP_OK);
    const app_status_t status = wait_status(true, false, 4000);
    assert(!status.enabled && status.fault == HEAT_FAULT_RESTART_LOCKOUT);
    wait_saved_heat_mode(HEAT_MODE_LOCKOUT);
    app_runtime_stop();
}

static void finish_blocked_save(esp_err_t expected)
{
    assert(xSemaphoreTake(setter_done, 0) == pdFALSE);
    atomic_store(&block_save, false);
    xSemaphoreGive(save_release);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(setter_result == expected);
}

static app_status_t wait_fault(heat_fault_t fault, uint32_t timeout)
{
    const uint64_t began = now_ms();
    app_status_t status;
    do {
        app_runtime_get_status(&status);
        if (status.fault == fault && !status.heater_on) return status;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (now_ms() - began < timeout);
    fprintf(stderr, "wanted fault=%d while save blocked; fault=%d heater=%d\n",
            fault, status.fault, status.heater_on);
    assert(!"blocked persistence delayed safety fault shutdown");
    return status;
}

static void request_fault_ack(void *arg)
{
    (void) arg;
    setter_result = app_runtime_ack_fault();
    xSemaphoreGive(setter_done);
    vTaskDelete(NULL);
}

static void restart_lockout_waits_for_off_commit(void)
{
    mode_save_entered = xSemaphoreCreateBinary();
    mode_save_release = xSemaphoreCreateBinary();
    setter_done = xSemaphoreCreateBinary();
    assert(mode_save_entered && mode_save_release && setter_done);
    blocked_mode = HEAT_MODE_OFF;
    atomic_store(&saved_heat_mode, HEAT_MODE_LOCKOUT);
    start_with_recovery(HEAT_MODE_LOCKOUT, true);
    assert(wait_status(true, false, 4000).fault == HEAT_FAULT_RESTART_LOCKOUT);

    assert(xTaskCreate(request_fault_ack, "ack_fault", 4096, NULL, 3, NULL) == pdPASS);
    assert(xSemaphoreTake(mode_save_entered, pdMS_TO_TICKS(1500)) == pdTRUE);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1500)) == pdTRUE);
    assert(setter_result == ESP_ERR_TIMEOUT);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_LOCKOUT);
    assert(wait_status(true, false, 1000).fault == HEAT_FAULT_RESTART_LOCKOUT);

    xSemaphoreGive(mode_save_release);
    assert(!wait_fault(HEAT_FAULT_NONE, 1500).enabled);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_OFF && !atomic_load(&heater));
    app_runtime_stop();
    vSemaphoreDelete(setter_done);
    vSemaphoreDelete(mode_save_release);
    vSemaphoreDelete(mode_save_entered);
}

static void zero_target_requires_relay_off(void)
{
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 4000);
    enable();
    atomic_store(&fail_output, true);

    assert(app_runtime_set_target_until(0.0f, now_ms() + 1500U) == ESP_FAIL);
    app_status_t status;
    app_runtime_get_status(&status);
    assert(status.target_c == 0.0f && !status.enabled && status.heater_on);
    assert(atomic_load(&saved_target) == 0.0f);

    atomic_store(&fail_output, false);
    assert(!wait_status(true, false, 1500).enabled);
    assert(app_runtime_set_target_until(0.0f, now_ms() + 1500U) == ESP_OK);
    app_runtime_stop();
}

static void failed_monitor_save_turns_off_the_relay(void)
{
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 4000);
    enable();
    wait_saved_heat_mode(HEAT_MODE_MANUAL_PENDING);
    atomic_store(&fail_next_heat_mode_save, true);
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_FAIL);
    assert(!atomic_load(&heater));
    assert(!wait_fault(HEAT_FAULT_NVS, 1500).enabled);
    assert(!app_runtime_monitor_active());
    wait_saved_heat_mode(HEAT_MODE_OFF);
    app_runtime_stop();
}

static app_status_t wait_calibration(float offset)
{
    const uint64_t began = now_ms();
    app_status_t status;
    do {
        app_runtime_get_status(&status);
        if (status.calibration_offset_c == offset) return status;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (now_ms() - began < 1000U);
    assert(!"calibration was not applied");
    return status;
}

static void calibration_applies_live_and_survives_restart(void)
{
    start();
    app_status_t status = wait_calibration(0.0f);
    assert(status.temperature.raw_c == 25.0f && status.temperature.filtered_c == 25.0f);
    assert(app_runtime_set_calibration(2.5f) == ESP_OK);
    status = wait_calibration(2.5f);
    assert(status.temperature.raw_c == 27.5f && status.temperature.filtered_c == 27.5f);
    assert(status.temperature.valid && status.temperature.consecutive_valid >= 3U);
    assert(!status.enabled && !status.heater_on && status.target_c == 60.0f);
    assert(atomic_load(&saved_calibration) == 2.5f);
    vTaskDelay(pdMS_TO_TICKS(600));
    status = wait_calibration(2.5f);
    assert(status.temperature.raw_c == 27.5f && status.temperature.filtered_c == 27.5f);

    assert(app_runtime_set_calibration(-1.5f) == ESP_OK);
    status = wait_calibration(-1.5f);
    assert(status.temperature.raw_c == 23.5f && status.temperature.filtered_c == 23.5f);
    const unsigned saves_before = atomic_load(&calibration_saves);
    const float invalid[] = {NAN, INFINITY, -INFINITY, -10.01f, 10.01f};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(app_runtime_set_calibration(invalid[i]) == ESP_ERR_INVALID_ARG);
    }
    assert(atomic_load(&calibration_saves) == saves_before);
    atomic_store(&save_result, ESP_FAIL);
    assert(app_runtime_set_calibration(4.0f) == ESP_FAIL);
    assert(wait_calibration(-1.5f).temperature.filtered_c == 23.5f);
    atomic_store(&save_result, ESP_OK);
    app_runtime_stop();
    start();
    status = wait_calibration(-1.5f);
    assert(status.temperature.filtered_c == 23.5f && !status.enabled);
    /* Printer monitoring never blocks manual settings. */
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
    assert(app_runtime_set_calibration(1.0f) == ESP_OK);
    app_runtime_monitor_end(true);
    (void) wait_status(true, false, 2000U);
    app_runtime_stop();
    atomic_store(&saved_calibration, 0.0f);
}

static void calibration_enforces_overtemperature_without_new_sensor_sample(void)
{
    atomic_store(&sensor_temperature_c, 75.0f);
    start();
    atomic_store(&stall_entered, false);
    atomic_store(&release_stall, false);
    atomic_store(&stall_sensor, true);
    const uint64_t began = now_ms();
    while (!atomic_load(&stall_entered) && now_ms() - began < 1000U) vTaskDelay(pdMS_TO_TICKS(10));
    assert(atomic_load(&stall_entered));
    assert(app_runtime_set_calibration(10.0f) == ESP_OK);
    const app_status_t status = wait_fault(HEAT_FAULT_OVERTEMP, 600U);
    assert(status.temperature.raw_c == 85.0f && !status.enabled);
    atomic_store(&stall_sensor, false);
    atomic_store(&release_stall, true);
    app_runtime_stop();
    atomic_store(&saved_calibration, 0.0f);
    atomic_store(&sensor_temperature_c, 25.0f);
}

static void blocked_save_does_not_delay_disable(void)
{
    start(); enable(); begin_blocked_save();
    const uint64_t began = now_ms();
    assert(app_runtime_set_enabled(false) == ESP_OK);
    (void) wait_status(false, false, 600);
    assert(now_ms() - began < 600 && !atomic_load(&heater));
    finish_blocked_save(ESP_OK);
    app_runtime_stop();
}

static void blocked_save_does_not_delay_sensor_fault(void)
{
    start(); enable(); begin_blocked_save();
    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_OPEN);
    const app_status_t status = wait_fault(HEAT_FAULT_SENSOR_OPEN, 6000);
    assert(!status.enabled && !status.fan_on && !atomic_load(&heater));
    finish_blocked_save(ESP_OK);
    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_NONE);
    app_runtime_stop();
}

static void blocked_save_does_not_delay_overtemperature(void)
{
    start(); enable(); begin_blocked_save();
    atomic_store(&sensor_temperature_c, 85.0f);
    /* The test includes both the one-second sample period and a control tick. */
    const app_status_t status = wait_fault(HEAT_FAULT_OVERTEMP, 2000);
    assert(!status.enabled && !atomic_load(&heater));
    finish_blocked_save(ESP_OK);
    atomic_store(&sensor_temperature_c, 25.0f);
    app_runtime_stop();
}

static void blocked_save_does_not_delay_stale_shutdown(void)
{
    atomic_store(&stall_entered, false);
    atomic_store(&release_stall, false);
    start(); enable(); begin_blocked_save();
    atomic_store(&stall_sensor, true);
    const app_status_t status = wait_fault(HEAT_FAULT_SENSOR_STALE, 3000);
    assert(!status.enabled && !atomic_load(&heater));
    finish_blocked_save(ESP_OK);
    atomic_store(&stall_sensor, false);
    atomic_store(&release_stall, true);
    app_runtime_stop();
}

static void blocked_save_reserves_its_command(void)
{
    start(); begin_blocked_save();
    assert(app_runtime_set_target(56.0f) == ESP_ERR_TIMEOUT);
    for (unsigned i = 0; i < 64; ++i) (void) app_runtime_report_startup_fault(HEAT_FAULT_NVS);
    finish_blocked_save(ESP_OK);
    const uint64_t began = now_ms();
    app_status_t status;
    do {
        app_runtime_get_status(&status);
        if (status.target_c == 55.0f) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (now_ms() - began < 1500);
    assert(status.target_c == 55.0f && !status.enabled);
    app_runtime_stop();
}

static void failed_save_releases_reservation_without_changing_target(void)
{
    start(); begin_blocked_save();
    atomic_store(&save_result, ESP_FAIL);
    finish_blocked_save(ESP_FAIL);
    atomic_store(&save_result, ESP_OK);
    app_status_t status; app_runtime_get_status(&status);
    assert(status.target_c == 60.0f && !status.enabled);
    assert(app_runtime_set_enabled(true) == ESP_OK);
    app_runtime_stop();
}

static void maintenance_waits_for_an_admitted_save_without_blocking_outputs(void)
{
    start(); begin_blocked_save();
    assert(app_runtime_begin_maintenance(500) == ESP_ERR_TIMEOUT);
    assert(app_runtime_maintenance_active() && !atomic_load(&heater));
    assert(app_runtime_end_maintenance() == ESP_ERR_NOT_ALLOWED);
    finish_blocked_save(ESP_OK);
    assert(app_runtime_begin_maintenance(1500) == ESP_OK);
    app_status_t status; app_runtime_get_status(&status);
    assert(status.target_c == 55.0f && !status.enabled);
    assert(app_runtime_end_maintenance() == ESP_OK);
    app_runtime_stop();
}

static void fault_survives_maintenance(heat_fault_t fault)
{
    start();
    if (fault == HEAT_FAULT_SENSOR_OPEN) atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_OPEN);
    else assert(app_runtime_report_startup_fault(fault) == ESP_OK);
    const uint64_t began = now_ms();
    app_status_t status;
    do {
        app_runtime_get_status(&status);
        if (status.state == HEAT_STATE_FAULT) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (now_ms() - began < 6000);
    assert(status.state == HEAT_STATE_FAULT && status.fault == fault);
    assert(app_runtime_begin_maintenance(1000) == ESP_OK);
    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_NONE);
    (void) wait_status(true, false, 4000);
    check_rejected();
    vTaskDelay(pdMS_TO_TICKS(200));
    assert(app_runtime_end_maintenance() == ESP_OK); /* failed-update cleanup */
    assert(app_runtime_set_enabled(true) == ESP_OK); /* queued; controller must reject */
    vTaskDelay(pdMS_TO_TICKS(300));
    app_runtime_get_status(&status);
    assert(status.state == HEAT_STATE_FAULT && status.fault == fault);
    assert(!status.enabled && !status.heater_on);
    assert(app_runtime_set_target(0) == ESP_OK);
    vTaskDelay(pdMS_TO_TICKS(300));
    app_runtime_get_status(&status);
    assert(status.state == HEAT_STATE_FAULT && status.fault == fault);
    app_runtime_stop();
}

static void conversion_wait_preserves_cadence_and_stop_response(void)
{
    atomic_store(&last_slow_read_ms, 0U);
    atomic_store(&slow_reads, 0U);
    atomic_store(&early_sample, false);
    atomic_store(&late_sample, false);
    atomic_store(&slow_sensor, true);
    start();
    assert(atomic_load(&slow_reads) >= 3);
    assert(!atomic_load(&late_sample));
    assert(!atomic_load(&early_sample));
    enable();
    app_status_t status;
    app_runtime_get_status(&status);
    uint64_t last_control_ms = status.uptime_ms;
    unsigned control_ticks = 0U;
    const uint64_t checking_since = now_ms();
    while (control_ticks < 3U && now_ms() - checking_since < 2200U) {
        app_runtime_get_status(&status);
        if (status.uptime_ms != last_control_ms) {
            const uint64_t interval_ms = status.uptime_ms - last_control_ms;
            assert(interval_ms >= 400U && interval_ms <= 750U);
            last_control_ms = status.uptime_ms;
            ++control_ticks;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    assert(control_ticks == 3U);
    const uint64_t began = now_ms();
    assert(app_runtime_set_enabled(false) == ESP_OK);
    (void) wait_status(false, false, 1000);
    assert(now_ms() - began < 750U);
    app_runtime_stop();
    atomic_store(&slow_sensor, false);
}

static app_status_t wait_failure_count(unsigned count, uint32_t timeout_ms)
{
    const uint64_t began = now_ms();
    app_status_t status;
    do {
        app_runtime_get_status(&status);
        if (status.temperature.consecutive_failures == count) return status;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (now_ms() - began < timeout_ms);
    fprintf(stderr, "wanted %u failed acquisitions, got %u (fault=%d)\n",
            count, status.temperature.consecutive_failures, status.fault);
    assert(!"sensor failure count did not reach expected value");
    return status;
}

static void sensor_retry_success_resets_count_and_fifth_failure_latches(void)
{
    start();
    enable();
    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_SHORT);
    app_status_t status = wait_failure_count(4U, 5500U);
    assert(status.heater_on && status.fan_on && status.enabled && status.fault == HEAT_FAULT_NONE);
    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_NONE);
    status = wait_failure_count(0U, 1800U);
    assert(status.temperature.valid && status.temperature.consecutive_valid == 1U);
    assert(status.heater_on && status.enabled && status.fault == HEAT_FAULT_NONE);

    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_OPEN);
    for (unsigned i = 1U; i <= 4U; ++i) {
        status = wait_failure_count(i, 1800U);
        assert(status.heater_on && status.enabled && status.fault == HEAT_FAULT_NONE);
    }
    status = wait_fault(HEAT_FAULT_SENSOR_OPEN, 1800U);
    assert(status.temperature.consecutive_failures == 5U);
    assert(!status.heater_on && !status.enabled && !status.fan_on);
    assert(app_runtime_ack_fault() == ESP_ERR_NOT_ALLOWED);
    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_NONE);
    status = wait_failure_count(0U, 1800U);
    assert(status.state == HEAT_STATE_FAULT && !status.heater_on);
    assert(app_runtime_ack_fault() == ESP_ERR_NOT_ALLOWED);
    (void) wait_status(true, false, 4000U);
    assert(app_runtime_ack_fault() == ESP_OK);
    app_runtime_get_status(&status);
    assert(status.state == HEAT_STATE_IDLE && !status.enabled && !status.heater_on && !status.fan_on);
    app_runtime_stop();
}

static void acquisition_stall_after_failed_read_still_shuts_down(void)
{
    start();
    enable();
    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_SHORT);
    app_status_t status = wait_failure_count(1U, 1800U);
    assert(status.heater_on && status.fault == HEAT_FAULT_NONE);
    atomic_store(&stall_sensor, true);
    status = wait_fault(HEAT_FAULT_SENSOR_STALE, 3200U);
    assert(status.temperature.consecutive_failures == 1U);
    assert(!status.enabled && !status.heater_on && !status.fan_on);
    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_NONE);
    atomic_store(&stall_sensor, false);
    atomic_store(&release_stall, true);
    app_runtime_stop();
}

static void stalled_acquisition_shuts_down_and_requires_three_fresh_samples(void)
{
    start();
    enable();
    atomic_store(&stall_sensor, true);
    const uint64_t began = now_ms();
    while (!atomic_load(&stall_entered) && now_ms() - began < 1500U) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    assert(atomic_load(&stall_entered));
    app_status_t status;
    app_runtime_get_status(&status);
    const uint64_t last_sample_ms = status.temperature.timestamp_ms;
    do {
        app_runtime_get_status(&status);
        if (status.fault == HEAT_FAULT_SENSOR_STALE) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (now_ms() - last_sample_ms < 2800U);
    assert(status.state == HEAT_STATE_FAULT && status.fault == HEAT_FAULT_SENSOR_STALE);
    assert(!status.heater_on && !status.fan_on && !status.enabled);
    assert(!status.temperature.valid && status.temperature.consecutive_valid == 0);

    atomic_store(&stall_sensor, false);
    atomic_store(&release_stall, true);
    const uint64_t resumed = now_ms();
    do {
        app_runtime_get_status(&status);
        if (status.temperature.valid && status.temperature.timestamp_ms > last_sample_ms) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (now_ms() - resumed < 1500U);
    /* The fast fake may publish two fresh readings before the controller's
     * next tick; neither one nor two may qualify fault acknowledgment. */
    assert(status.temperature.valid && status.temperature.consecutive_valid >= 1 &&
           status.temperature.consecutive_valid < 3);
    atomic_store(&release_stall, false);
    atomic_store(&stall_sensor, true);
    assert(app_runtime_ack_fault() == ESP_ERR_NOT_ALLOWED);
    vTaskDelay(pdMS_TO_TICKS(150));
    app_runtime_get_status(&status);
    assert(status.state == HEAT_STATE_FAULT && !status.heater_on);
    atomic_store(&stall_sensor, false);
    atomic_store(&release_stall, true);
    (void) wait_status(true, false, 4000);
    assert(app_runtime_ack_fault() == ESP_OK);
    vTaskDelay(pdMS_TO_TICKS(200));
    app_runtime_get_status(&status);
    assert(status.state == HEAT_STATE_IDLE && !status.enabled && !status.heater_on);
    app_runtime_stop();
}

static app_status_t wait_temperature_below(float threshold)
{
    const uint64_t began=now_ms();
    app_status_t status;
    do {
        app_runtime_get_status(&status);
        if(status.temperature.valid && status.temperature.consecutive_valid>=3 &&
           status.temperature.raw_c<threshold && status.temperature.filtered_c<threshold)return status;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while(now_ms()-began<4000);
    assert(!"temperature did not recover");return status;
}

static void acknowledgement_reports_controller_decision(const char *scenario)
{
    start();
    heat_fault_t fault=HEAT_FAULT_NVS;
    if(!strcmp(scenario,"ack_output"))fault=HEAT_FAULT_OUTPUT_INIT;
    else if(!strcmp(scenario,"ack_sensor"))fault=HEAT_FAULT_SENSOR_OPEN;
    else if(!strcmp(scenario,"ack_stale"))fault=HEAT_FAULT_SENSOR_STALE;
    else if(!strcmp(scenario,"ack_overtemp"))fault=HEAT_FAULT_OVERTEMP;
    if(fault==HEAT_FAULT_SENSOR_OPEN)atomic_store(&sensor_fault,TEMPERATURE_SENSOR_FAULT_OPEN);
    else if(fault==HEAT_FAULT_SENSOR_STALE)atomic_store(&stall_sensor,true);
    else if(fault==HEAT_FAULT_OVERTEMP)atomic_store(&sensor_temperature_c,85);
    else assert(app_runtime_report_startup_fault(fault)==ESP_OK);
    (void)wait_fault(fault, fault == HEAT_FAULT_SENSOR_OPEN ? 6000U : 3500U);

    if(!strcmp(scenario,"ack_expired") || !strcmp(scenario,"ack_early_timeout")) {
        host_control_queue_pause(true);
        const uint64_t began=now_ms();
        while(!host_control_queue_is_paused() && now_ms()-began<1000)vTaskDelay(pdMS_TO_TICKS(10));
        assert(host_control_queue_is_paused());
        if(!strcmp(scenario,"ack_early_timeout"))host_reply_queue_timeout_early();
        const uint64_t requested=now_ms();
        assert(app_runtime_ack_fault()==ESP_ERR_TIMEOUT);
        assert(now_ms()-requested>=500);
        host_control_queue_pause(false);
        vTaskDelay(pdMS_TO_TICKS(250));
        app_status_t status;app_runtime_get_status(&status);
        assert(status.state==HEAT_STATE_FAULT && status.fault==HEAT_FAULT_NVS);
    } else if(!strcmp(scenario,"ack_changed_fault")) {
        atomic_store(&sensor_temperature_c,85);
        const uint64_t began=now_ms();
        app_status_t status;
        do {
            app_runtime_get_status(&status);
            if(status.temperature.raw_c>=85)break;
            vTaskDelay(pdMS_TO_TICKS(20));
        } while(now_ms()-began<1500);
        assert(status.temperature.raw_c>=85 && status.fault==HEAT_FAULT_NVS);
        assert(app_runtime_ack_fault()==ESP_ERR_NOT_ALLOWED);
        app_runtime_get_status(&status);
        assert(status.state==HEAT_STATE_FAULT && status.fault==HEAT_FAULT_OVERTEMP);
        atomic_store(&sensor_temperature_c,25);
        (void)wait_temperature_below(80);
    } else if(fault!=HEAT_FAULT_NVS) {
        assert(app_runtime_ack_fault()==ESP_ERR_NOT_ALLOWED);
        app_status_t status;app_runtime_get_status(&status);
        assert(status.state==HEAT_STATE_FAULT && !status.enabled && !status.heater_on);
        if(fault==HEAT_FAULT_OUTPUT_INIT){app_runtime_stop();return;}
        atomic_store(&sensor_fault,TEMPERATURE_SENSOR_FAULT_NONE);
        atomic_store(&sensor_temperature_c,25);
        atomic_store(&stall_sensor,false);
        atomic_store(&release_stall,true);
        (void)wait_temperature_below(80);
    }
    assert(app_runtime_ack_fault()==ESP_OK);
    app_status_t status;app_runtime_get_status(&status);
    assert(status.state==HEAT_STATE_IDLE && status.fault==HEAT_FAULT_NONE);
    assert(!status.enabled && !status.heater_on && !atomic_load(&heater));
    app_runtime_stop();
}

static void runtime_records_transition_history(void)
{
    start();
    wait_initial_relay_interval();
    diagnostic_record_t records[DIAGNOSTICS_CAPACITY];size_t count=0;
    diagnostics_snapshot(records,DIAGNOSTICS_CAPACITY,&count);
    assert(count==1 && records[0].from_state==HEAT_STATE_BOOT && records[0].to_state==HEAT_STATE_IDLE);
    assert(records[0].fault==HEAT_FAULT_NONE && records[0].timestamp_ms>0);
    enable();
    atomic_store(&sensor_fault,TEMPERATURE_SENSOR_FAULT_OPEN);
    (void)wait_fault(HEAT_FAULT_SENSOR_OPEN,6000);
    diagnostics_snapshot(records,DIAGNOSTICS_CAPACITY,&count);
    assert(count==3 && records[1].from_state==HEAT_STATE_IDLE && records[1].to_state==HEAT_STATE_HEATING);
    assert(records[2].from_state==HEAT_STATE_HEATING && records[2].to_state==HEAT_STATE_FAULT);
    assert(records[2].fault==HEAT_FAULT_SENSOR_OPEN);
    vTaskDelay(pdMS_TO_TICKS(400));
    diagnostics_snapshot(records,DIAGNOSTICS_CAPACITY,&count);assert(count==3);

    assert(app_runtime_report_startup_fault(HEAT_FAULT_NVS)==ESP_OK);
    (void)wait_fault(HEAT_FAULT_NVS,1000);
    diagnostics_snapshot(records,DIAGNOSTICS_CAPACITY,&count);
    assert(count==4 && records[3].from_state==HEAT_STATE_FAULT && records[3].to_state==HEAT_STATE_FAULT);
    assert(records[3].fault==HEAT_FAULT_NVS);
    atomic_store(&sensor_fault,TEMPERATURE_SENSOR_FAULT_NONE);
    (void)wait_temperature_below(80);
    assert(app_runtime_ack_fault()==ESP_OK);
    diagnostics_snapshot(records,DIAGNOSTICS_CAPACITY,&count);
    assert(count==5 && records[4].from_state==HEAT_STATE_FAULT && records[4].to_state==HEAT_STATE_IDLE);
    assert(records[4].fault==HEAT_FAULT_NONE);
    for(size_t i=1;i<count;++i)assert(records[i].timestamp_ms>=records[i-1].timestamp_ms);
    vTaskDelay(pdMS_TO_TICKS(300));
    diagnostics_snapshot(records,DIAGNOSTICS_CAPACITY,&count);assert(count==5);
    app_runtime_stop();
}

static void wait_monitor_inactive(uint32_t timeout)
{
    const uint64_t start_ms = now_ms();
    while (app_runtime_monitor_active()) {
        assert(now_ms() - start_ms < timeout);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static void monitor_requires_enabled_heating(void)
{
    start();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_ERR_NOT_ALLOWED);
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 6000U) == ESP_ERR_INVALID_ARG);
    assert(!app_runtime_monitor_active());
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
    assert(app_runtime_monitor_active());
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_monitor_permit(PRINTER_MONITOR_WATCHING, now_ms() + 6000U) == ESP_ERR_INVALID_ARG);
    app_runtime_monitor_end(false);
    assert(app_runtime_monitor_permit(PRINTER_MONITOR_WATCHING, now_ms() + 1000U) == ESP_ERR_INVALID_STATE);
    app_runtime_stop();
}
static void monitor_expired_permit_holds_heater(void)
{
    start();
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 1000U) == ESP_OK);
    const app_status_t held = wait_status(true, false, 3000);
    assert(held.enabled);
    assert(!held.fan_on);
    bool heating = false;
    for (unsigned i = 0; i < 40 && !heating; ++i) {
        assert(app_runtime_monitor_permit(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
        vTaskDelay(pdMS_TO_TICKS(200));
        heating = atomic_load(&heater);
    }
    assert(heating);
    app_runtime_stop();
}
static void monitor_disable_then_enable(void)
{
    start();
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, 0) == ESP_OK);
    (void) wait_status(true, false, 2000);
    assert(app_runtime_set_enabled(false) == ESP_OK);
    wait_monitor_inactive(1000);
    enable();
    app_runtime_stop();
}
static void monitor_end_choices(void)
{
    start();
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
    app_runtime_monitor_end(false);
    assert(!app_runtime_monitor_active());
    vTaskDelay(pdMS_TO_TICKS(6000)); /* the old permit would have expired */
    app_status_t status;
    app_runtime_get_status(&status);
    assert(status.enabled && atomic_load(&heater));
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
    app_runtime_monitor_end(true);
    assert(!app_runtime_monitor_active());
    status = wait_status(true, false, 2000);
    assert(!status.enabled);
    app_runtime_stop();
}
static void monitor_disable_and_enable_in_one_iteration(void)
{
    start();
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, 0) == ESP_OK);
    (void) wait_status(true, false, 2000);
    host_control_queue_pause(true);
    const uint64_t began = now_ms();
    while (!host_control_queue_is_paused() && now_ms() - began < 1000)
        vTaskDelay(pdMS_TO_TICKS(10));
    assert(host_control_queue_is_paused());
    assert(app_runtime_set_enabled(false) == ESP_OK);
    assert(app_runtime_set_enabled(true) == ESP_OK);
    host_control_queue_pause(false);
    wait_monitor_inactive(1000);
    const app_status_t status = wait_status(true, true, 7000);
    assert(status.enabled);
    assert(app_runtime_monitor_permit(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_ERR_INVALID_STATE);
    app_runtime_stop();
}
static void monitor_maintenance_ends(void)
{
    start();
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
    assert(app_runtime_begin_maintenance(15000) == ESP_OK);
    assert(!app_runtime_monitor_active());
    assert(app_runtime_end_maintenance() == ESP_OK);
    app_runtime_stop();
}

static void monitor_stale_end_preserves_manual_heating(void)
{
    start();
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
    assert(app_runtime_set_enabled(false) == ESP_OK);
    wait_monitor_inactive(1000);
    enable();
    /* A printer completion resumes after a newer manual heating session. */
    app_runtime_monitor_end(true);
    vTaskDelay(pdMS_TO_TICKS(300));
    app_status_t status;
    app_runtime_get_status(&status);
    assert(status.enabled && status.heater_on && !app_runtime_monitor_active());
    app_runtime_stop();
}

static void monitor_pending_end_preserves_later_enable(void)
{
    start();
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, now_ms() + 5000U) == ESP_OK);
    host_control_queue_pause(true);
    const uint64_t began = now_ms();
    while (!host_control_queue_is_paused() && now_ms() - began < 1000U)
        vTaskDelay(pdMS_TO_TICKS(10));
    assert(host_control_queue_is_paused());
    assert(app_runtime_set_enabled(false) == ESP_OK);
    assert(app_runtime_set_enabled(true) == ESP_OK);
    assert(!app_runtime_monitor_active());
    /* Disable invalidates the old monitor before the controller consumes it. */
    app_runtime_monitor_end(true);
    host_control_queue_pause(false);
    wait_monitor_inactive(1000);
    vTaskDelay(pdMS_TO_TICKS(300));
    app_status_t status;
    app_runtime_get_status(&status);
    assert(status.enabled && !status.heater_on && status.state == HEAT_STATE_HOLDING);
    (void) wait_status(true, true, 7000);
    app_runtime_stop();
}

static void blocked_remote_setter(void *arg)
{
    (void) arg;
    setter_result = app_runtime_set_target_until(55.0f, now_ms() + 1000U);
    xSemaphoreGive(setter_done);
    vTaskDelete(NULL);
}

static void remote_heat_commands_respect_deadlines_and_confirm_state(void)
{
    start();
    wait_initial_relay_interval();
    assert(app_runtime_set_target_until(55.0f, now_ms() + 1000U) == ESP_OK);
    app_status_t status;
    app_runtime_get_status(&status);
    assert(status.target_c == 55.0f && !status.enabled);

    host_control_queue_pause(true);
    const uint64_t began = now_ms();
    while (!host_control_queue_is_paused() && now_ms() - began < 1000U)
        vTaskDelay(pdMS_TO_TICKS(10));
    assert(host_control_queue_is_paused());
    assert(app_runtime_set_enabled_until(true, now_ms() + 100U) == ESP_ERR_TIMEOUT);
    host_control_queue_pause(false);
    vTaskDelay(pdMS_TO_TICKS(250));
    assert(!atomic_load(&heater));
    app_runtime_get_status(&status);
    assert(!status.enabled);

    assert(app_runtime_set_enabled_until(true, now_ms() + 1000U) == ESP_OK);
    app_runtime_get_status(&status);
    assert(status.enabled && status.heater_on);
    assert(app_runtime_set_enabled_until(false, now_ms() + 1000U) == ESP_OK);
    app_runtime_get_status(&status);
    assert(!status.enabled && !status.heater_on && !status.fan_on);

    assert(app_runtime_set_target_until(20.0f, now_ms() + 1000U) == ESP_OK);
    assert(app_runtime_set_enabled_until(true, now_ms() + 1000U) == ESP_OK);
    const unsigned target_writes = atomic_load(&saves);
    const unsigned calibration_writes = atomic_load(&calibration_saves);
    host_control_queue_pause(true);
    const uint64_t paused_at = now_ms();
    while (!host_control_queue_is_paused() && now_ms() - paused_at < 1000U)
        vTaskDelay(pdMS_TO_TICKS(10));
    assert(host_control_queue_is_paused());
    assert(app_runtime_set_target_until(55.0f, now_ms() + 100U) == ESP_ERR_TIMEOUT);
    assert(app_runtime_set_calibration_until(-10.0f, now_ms() + 100U) == ESP_ERR_TIMEOUT);
    host_control_queue_pause(false);
    vTaskDelay(pdMS_TO_TICKS(250));
    app_runtime_get_status(&status);
    assert(status.enabled && !status.heater_on && status.target_c == 20.0f);
    assert(status.calibration_offset_c == 0.0f && atomic_load(&saved_target) == 20.0f);
    assert(atomic_load(&saves) == target_writes && atomic_load(&calibration_saves) == calibration_writes);

    atomic_store(&fail_next_target_save_after_write, true);
    begin_blocked_save_using(blocked_remote_setter);
    app_runtime_get_status(&status);
    assert(status.enabled && !status.heater_on && !atomic_load(&heater));
    assert(status.target_c == 20.0f && atomic_load(&saved_target) == 20.0f);
    finish_blocked_save(ESP_FAIL);
    app_runtime_get_status(&status);
    assert(status.fault == HEAT_FAULT_NVS && !status.enabled && !status.heater_on);
    assert(status.target_c == 20.0f && atomic_load(&saved_target) == 20.0f);

    assert(app_runtime_set_calibration_until(1.5f, now_ms() + 1000U) == ESP_OK);
    app_runtime_get_status(&status);
    assert(status.calibration_offset_c == 1.5f && atomic_load(&saved_calibration) == 1.5f);
    app_runtime_stop();
}

static void rejected_enable_stays_off_when_relay_recovers(void)
{
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 5000);
    wait_initial_relay_interval();
    atomic_store(&fail_output_on, true);
    assert(app_runtime_set_enabled_until(true, now_ms() + 1000U) == ESP_ERR_NOT_ALLOWED);

    app_status_t status;
    app_runtime_get_status(&status);
    assert(!status.enabled && !status.heater_on && !status.restore_pending);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_OFF);
    atomic_store(&fail_output_on, false);
    vTaskDelay(pdMS_TO_TICKS(700));
    assert(!atomic_load(&heater));

    /* When neither GPIO write succeeds, keep the durable lockout marker
     * until the relay is confirmed off. */
    atomic_store(&fail_output, true);
    assert(app_runtime_set_enabled_until(true, now_ms() + 1000U) == ESP_ERR_TIMEOUT);
    app_runtime_get_status(&status);
    assert(!status.enabled && atomic_load(&saved_heat_mode) == HEAT_MODE_MANUAL_PENDING);
    atomic_store(&fail_output, false);
    wait_saved_heat_mode(HEAT_MODE_OFF);
    enable();
    app_runtime_stop();
}

static void unlock_remote_enable(void)
{
    xSemaphoreGive(remote_enable_lock);
}

static bool lock_remote_enable_if_current(uint32_t revision)
{
    assert(xSemaphoreTake(remote_enable_lock, portMAX_DELAY) == pdTRUE);
    if (revision == remote_revision) return true;
    unlock_remote_enable();
    return false;
}

static bool lock_remote_relay_start(void)
{
    assert(xSemaphoreTake(remote_enable_lock, portMAX_DELAY) == pdTRUE);
    if (!disable_pending) return true;
    atomic_fetch_add(&blocked_relay_starts, 1U);
    unlock_remote_enable();
    return false;
}

static const app_runtime_enable_guard_t remote_enable_guard = {
    .lock_if_current = lock_remote_enable_if_current,
    .unlock = unlock_remote_enable,
    .lock_relay_start = lock_remote_relay_start,
    .unlock_relay_start = unlock_remote_enable,
};

static void set_remote_revision(uint32_t revision)
{
    assert(xSemaphoreTake(remote_enable_lock, pdMS_TO_TICKS(250)) == pdTRUE);
    remote_revision = revision;
    unlock_remote_enable();
}

static void request_guarded_enable(void *arg)
{
    (void) arg;
    setter_result = app_runtime_enable_if_current(now_ms() + 1000U, &remote_enable_guard, 1);
    xSemaphoreGive(setter_done);
    vTaskDelete(NULL);
}

static void deferred_relay_start_waits_for_pending_disable(void)
{
    remote_enable_lock = xSemaphoreCreateMutex();
    assert(remote_enable_lock);
    start_with_recovery(HEAT_MODE_OFF, true);
    app_runtime_set_relay_start_guard(&remote_enable_guard);
    (void) wait_status(true, false, 4000);
    assert(app_runtime_enable_if_current(now_ms() + 1000U, &remote_enable_guard, 0) == ESP_OK);
    assert(!atomic_load(&heater));

    assert(xSemaphoreTake(remote_enable_lock, portMAX_DELAY) == pdTRUE);
    disable_pending = true;
    remote_revision = 1;
    unlock_remote_enable();
    const uint64_t deadline = now_ms() + 5500U;
    while (!atomic_load(&blocked_relay_starts) && now_ms() < deadline) vTaskDelay(1);
    assert(atomic_load(&blocked_relay_starts) > 0U);
    assert(!atomic_load(&heater) && atomic_load(&relay_activations) == 0U);

    assert(app_runtime_set_enabled_until(false, now_ms() + 1000U) == ESP_OK);
    assert(xSemaphoreTake(remote_enable_lock, portMAX_DELAY) == pdTRUE);
    disable_pending = false;
    unlock_remote_enable();
    assert(!wait_status(true, false, 1000).enabled);
    assert(app_runtime_enable_if_current(now_ms() + 1000U, &remote_enable_guard, 1) == ESP_OK);
    (void) wait_status(true, true, 3500);
    app_runtime_stop();
    vSemaphoreDelete(remote_enable_lock);
}

static void cancelled_remote_enable_stays_off(void)
{
    remote_enable_lock = xSemaphoreCreateMutex();
    assert(remote_enable_lock);
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 5000);
    wait_initial_relay_interval();
    setter_done = xSemaphoreCreateBinary();
    assert(setter_done);
    const unsigned activations = atomic_load(&relay_activations);

    set_remote_revision(1);
    host_control_queue_pause(true);
    const uint64_t pause_deadline = now_ms() + 1000U;
    while (!host_control_queue_is_paused() && now_ms() < pause_deadline) vTaskDelay(1);
    assert(host_control_queue_is_paused());
    assert(xTaskCreate(request_guarded_enable, "enable", 4096, NULL, 5, NULL) == pdPASS);
    const uint64_t queue_deadline = now_ms() + 500U;
    while (!host_control_queue_has_command() && now_ms() < queue_deadline) vTaskDelay(1);
    assert(host_control_queue_has_command());
    set_remote_revision(2);
    host_control_queue_pause(false);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(setter_result == ESP_ERR_NOT_ALLOWED);
    assert(atomic_load(&relay_activations) == activations);
    assert(atomic_load(&heat_mode_saves) == 0);

    set_remote_revision(1);
    blocked_mode = HEAT_MODE_MANUAL_PENDING;
    mode_save_entered = xSemaphoreCreateBinary();
    mode_save_release = xSemaphoreCreateBinary();
    assert(mode_save_entered && mode_save_release);
    assert(xTaskCreate(request_guarded_enable, "enable", 4096, NULL, 5, NULL) == pdPASS);
    assert(xSemaphoreTake(mode_save_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
    set_remote_revision(2);
    xSemaphoreGive(mode_save_release);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(setter_result == ESP_ERR_NOT_ALLOWED);
    assert(atomic_load(&relay_activations) == activations);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_OFF);
    assert(atomic_load(&heat_mode_saves) == 2);
    app_status_t status;
    app_runtime_get_status(&status);
    assert(!status.enabled && !status.heater_on);

    vSemaphoreDelete(mode_save_entered);
    vSemaphoreDelete(mode_save_release);
    mode_save_entered = mode_save_release = NULL;
    assert(app_runtime_enable_if_current(now_ms() + 1000U, &remote_enable_guard, 2) == ESP_OK);
    (void) wait_status(true, true, 7000);
    app_runtime_stop();
    vSemaphoreDelete(remote_enable_lock);
    vSemaphoreDelete(setter_done);
}

static void expired_remote_enable_rolls_back(void)
{
    remote_enable_lock = xSemaphoreCreateMutex();
    setter_done = xSemaphoreCreateBinary();
    mode_save_entered = xSemaphoreCreateBinary();
    mode_save_release = xSemaphoreCreateBinary();
    assert(remote_enable_lock && setter_done && mode_save_entered && mode_save_release);
    blocked_mode = HEAT_MODE_MANUAL_PENDING;
    block_after_mode_write = true;
    set_remote_revision(1);
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 5000);
    const unsigned activations = atomic_load(&relay_activations);
    const unsigned writes = atomic_load(&heat_mode_saves);

    assert(xTaskCreate(request_guarded_enable, "enable", 4096, NULL, 5, NULL) == pdPASS);
    assert(xSemaphoreTake(mode_save_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(2000)) == pdTRUE);
    assert(setter_result == ESP_ERR_TIMEOUT);
    const heat_mode_t mode_at_power_cut = atomic_load(&saved_heat_mode);
    assert(mode_at_power_cut == HEAT_MODE_MANUAL_PENDING);
    assert(!atomic_load(&heater));
    xSemaphoreGive(mode_save_release);

    const uint64_t deadline = now_ms() + 2000U;
    while (atomic_load(&heat_mode_saves) < writes + 2 && now_ms() < deadline) vTaskDelay(1);
    assert(atomic_load(&heat_mode_saves) == writes + 2);
    app_runtime_stop();
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_OFF);
    assert(atomic_load(&relay_activations) == activations);
    vSemaphoreDelete(remote_enable_lock);
    vSemaphoreDelete(setter_done);
    vSemaphoreDelete(mode_save_entered);
    vSemaphoreDelete(mode_save_release);
    mode_save_entered = mode_save_release = NULL;
    remote_enable_lock = NULL;

    /* Restore the flash contents from before the controller could roll back. */
    atomic_store(&saved_heat_mode, mode_at_power_cut);
    assert(app_runtime_start() == ESP_OK);
    const app_status_t restored = wait_status(true, false, 5000);
    assert(!restored.enabled && !restored.restore_pending);
    assert(restored.fault == HEAT_FAULT_RESTART_LOCKOUT);
    assert(atomic_load(&relay_activations) == activations);
    wait_saved_heat_mode(HEAT_MODE_LOCKOUT);
    app_runtime_stop();
}

static void overtemperature_publishes_before_flash_and_survives_power_loss(void)
{
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 5000);
    enable();
    blocked_mode = HEAT_MODE_LOCKOUT;
    mode_save_entered = xSemaphoreCreateBinary();
    mode_save_release = xSemaphoreCreateBinary();
    assert(mode_save_entered && mode_save_release);

    atomic_store(&sensor_temperature_c, 85.0f);
    assert(xSemaphoreTake(mode_save_entered, pdMS_TO_TICKS(3000)) == pdTRUE);
    assert(!atomic_load(&heater));
    const heat_mode_t interrupted_mode = atomic_load(&saved_heat_mode);
    assert(interrupted_mode == HEAT_MODE_MANUAL_PENDING);
    const app_status_t cutoff = wait_fault(HEAT_FAULT_OVERTEMP, 200);
    assert(!cutoff.enabled);
    const uint64_t deadline = now_ms() + 2500U;
    app_status_t status;
    do {
        vTaskDelay(pdMS_TO_TICKS(20));
        app_runtime_get_status(&status);
    } while (status.temperature.timestamp_ms <= cutoff.temperature.timestamp_ms && now_ms() < deadline);
    assert(status.temperature.timestamp_ms > cutoff.temperature.timestamp_ms);
    assert(status.fault == HEAT_FAULT_OVERTEMP && !status.heater_on);

    xSemaphoreGive(mode_save_release);
    wait_saved_heat_mode(HEAT_MODE_LOCKOUT);
    app_runtime_stop();
    vSemaphoreDelete(mode_save_entered);
    vSemaphoreDelete(mode_save_release);
    mode_save_entered = mode_save_release = NULL;

    atomic_store(&sensor_temperature_c, 25.0f);
    saved_heat_requires_acknowledgement(interrupted_mode);
    saved_heat_requires_acknowledgement(HEAT_MODE_LOCKOUT);
}

static void begin_blocked_monitor(void *arg)
{
    (void) arg;
    setter_result = app_runtime_monitor_begin(PRINTER_MONITOR_WAITING, now_ms() + 5000U);
    xSemaphoreGive(setter_done);
    vTaskDelete(NULL);
}

static void remote_disable_turns_off_during_mode_save(void)
{
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 5000);
    enable();
    mode_save_entered = xSemaphoreCreateBinary();
    mode_save_release = xSemaphoreCreateBinary();
    setter_done = xSemaphoreCreateBinary();
    assert(mode_save_entered && mode_save_release && setter_done);
    assert(xTaskCreate(begin_blocked_monitor, "mode_save", 4096, NULL, 3, NULL) == pdPASS);
    assert(xSemaphoreTake(mode_save_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(atomic_load(&heater));

    /* Acknowledgement needs a durable OFF record. The relay must switch off
     * immediately, including when the command times out waiting for flash. */
    assert(app_runtime_set_enabled_until(false, now_ms() + 40U) == ESP_ERR_TIMEOUT);
    assert(!atomic_load(&heater) && !app_runtime_monitor_active());
    assert(app_runtime_set_enabled_until(true, now_ms() + 1000U) == ESP_ERR_NOT_ALLOWED);
    xSemaphoreGive(mode_save_release);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(setter_result == ESP_ERR_INVALID_STATE);
    const uint64_t save_deadline = now_ms() + 2000U;
    while (atomic_load(&saved_heat_mode) != HEAT_MODE_OFF && now_ms() < save_deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    app_status_t status;
    app_runtime_get_status(&status);
    assert(!status.enabled && !status.heater_on && !app_runtime_monitor_active());
    assert(!atomic_load(&heater) && atomic_load(&saved_heat_mode) == HEAT_MODE_OFF);
    app_runtime_stop();
}

typedef struct {
    printer_monitor_phase_t phase;
    uint64_t permit_until_ms;
} requested_permit_t;

static void renew_blocked_monitor(void *arg)
{
    const requested_permit_t *request = arg;
    setter_result = app_runtime_monitor_permit(request->phase, request->permit_until_ms);
    xSemaphoreGive(setter_done);
    vTaskDelete(NULL);
}

static void stalled_monitor_save_respects_permit(bool hold_now)
{
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 5000);
    enable();
    const uint64_t previous_permit = now_ms() + (hold_now ? 5000U : 800U);
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WAITING, previous_permit) == ESP_OK);

    blocked_mode = HEAT_MODE_MONITOR_WATCHING;
    mode_save_entered = xSemaphoreCreateBinary();
    mode_save_release = xSemaphoreCreateBinary();
    setter_done = xSemaphoreCreateBinary();
    assert(mode_save_entered && mode_save_release && setter_done);
    requested_permit_t request = {
        .phase = PRINTER_MONITOR_WATCHING,
        .permit_until_ms = hold_now ? 0 : now_ms() + 5000U,
    };
    assert(xTaskCreate(renew_blocked_monitor, "phase_save", 4096, &request, 3, NULL) == pdPASS);
    assert(xSemaphoreTake(mode_save_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
    if (hold_now) {
        /* A pause switches the relay off before the phase write even begins. */
        assert(!atomic_load(&heater));
    } else {
        assert(atomic_load(&heater));
        assert(wait_status(false, false, 2200).enabled);
        assert(now_ms() >= previous_permit);
        assert(!atomic_load(&heater));
    }
    assert(xSemaphoreTake(setter_done, 0) == pdFALSE);
    xSemaphoreGive(mode_save_release);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(setter_result == ESP_OK);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_MONITOR_WATCHING);
    assert(app_runtime_monitor_end(true) == ESP_OK);
    app_runtime_stop();
}

static void remote_monitor_stop_expires_at_runtime_lock(void)
{
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 5000);
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WAITING, 0) == ESP_OK);
    assert(!atomic_load(&heater));
    assert(app_runtime_monitor_suspend_recovery() == ESP_OK);

    mode_save_entered = xSemaphoreCreateBinary();
    mode_save_release = xSemaphoreCreateBinary();
    setter_done = xSemaphoreCreateBinary();
    assert(mode_save_entered && mode_save_release && setter_done);
    requested_permit_t request = {.phase = PRINTER_MONITOR_WAITING};
    assert(xTaskCreate(renew_blocked_monitor, "renew_monitor", 4096, &request, 3, NULL) == pdPASS);
    assert(xSemaphoreTake(mode_save_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(app_runtime_monitor_end_until(false, now_ms() + 40U, NULL, 0) == ESP_ERR_TIMEOUT);
    xSemaphoreGive(mode_save_release);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(setter_result == ESP_OK);
    assert(app_runtime_monitor_phase() == PRINTER_MONITOR_WAITING && !atomic_load(&heater));
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_MONITOR_WAITING);
    assert(app_runtime_monitor_end_until(false, now_ms() + 1000U, NULL, 0) == ESP_OK);
    assert(!app_runtime_monitor_active());
    wait_saved_heat_mode(HEAT_MODE_MANUAL_PENDING);
    app_runtime_stop();
}

static void request_guarded_monitor_stop(void *arg)
{
    const uint64_t deadline = *(const uint64_t *) arg;
    setter_result = app_runtime_monitor_end_until(false, deadline, &remote_enable_guard, 1);
    xSemaphoreGive(setter_done);
    vTaskDelete(NULL);
}

static void interrupted_remote_monitor_stop_keeps_hold(bool expires)
{
    start_with_recovery(HEAT_MODE_OFF, true);
    (void) wait_status(true, false, 5000);
    enable();
    assert(app_runtime_monitor_begin(PRINTER_MONITOR_WATCHING, 0) == ESP_OK);
    (void) wait_status(true, false, 2000);
    const unsigned activations = atomic_load(&relay_activations);
    const unsigned writes = atomic_load(&heat_mode_saves);

    remote_enable_lock = xSemaphoreCreateMutex();
    assert(remote_enable_lock);
    set_remote_revision(2);
    assert(app_runtime_monitor_end_until(false, now_ms() + 1000U, &remote_enable_guard, 1) ==
           ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_monitor_phase() == PRINTER_MONITOR_WATCHING);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_MONITOR_WATCHING);
    assert(atomic_load(&heat_mode_saves) == writes);

    set_remote_revision(1);
    blocked_mode = HEAT_MODE_MANUAL_PENDING;
    mode_save_entered = xSemaphoreCreateBinary();
    mode_save_release = xSemaphoreCreateBinary();
    setter_done = xSemaphoreCreateBinary();
    assert(mode_save_entered && mode_save_release && setter_done);
    uint64_t deadline = now_ms() + 1000U;
    assert(xTaskCreate(request_guarded_monitor_stop, "stop_monitor", 4096, &deadline, 3, NULL) == pdPASS);
    assert(xSemaphoreTake(mode_save_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
    if (expires) {
        while (now_ms() < deadline) vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        set_remote_revision(2);
    }
    xSemaphoreGive(mode_save_release);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(setter_result == (expires ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_ALLOWED));
    assert(app_runtime_monitor_phase() == PRINTER_MONITOR_WATCHING);
    assert(atomic_load(&saved_heat_mode) == HEAT_MODE_MONITOR_WATCHING);
    assert(atomic_load(&heat_mode_saves) == writes + 2);
    assert(!atomic_load(&heater) && atomic_load(&relay_activations) == activations);

    app_runtime_stop();
    vSemaphoreDelete(mode_save_entered);
    vSemaphoreDelete(mode_save_release);
    vSemaphoreDelete(setter_done);
    vSemaphoreDelete(remote_enable_lock);
}

static void restart_setting_uses_the_queued_target(void)
{
    start_with_recovery(HEAT_MODE_OFF, false);
    /* The unconfirmed target save can still be in the queue when Restart arrives. */
    assert(app_runtime_set_target(55.0f) == ESP_OK);
    assert(app_runtime_set_restart_until(57.0f, now_ms() + 1000U) == ESP_ERR_NOT_ALLOWED);
    assert(atomic_load(&saved_hysteresis) == 1.0f);
    assert(app_runtime_set_restart_until(50.5f, now_ms() + 1000U) == ESP_OK);
    assert(atomic_load(&saved_hysteresis) == 4.5f);
    app_status_t status;
    app_runtime_get_status(&status);
    assert(status.target_c == 55.0f && status.restart_c == 50.5f && !status.heater_on);
    assert(app_runtime_set_target_until(60.0f, now_ms() + 1000U) == ESP_OK);
    app_runtime_get_status(&status);
    assert(status.restart_c == 55.5f && !status.enabled);
    app_runtime_stop();
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc == 2) {
        if (strcmp(argv[1], "monitor_recovery") == 0) monitored_heating_ends_on_restart();
        else if (strcmp(argv[1], "legacy_mode_lockout") == 0) saved_heat_requires_acknowledgement(HEAT_MODE_MANUAL);
        else if (strcmp(argv[1], "startup_storage_lockout") == 0) startup_storage_fault_preserves_restart_lockout();
        else if (strcmp(argv[1], "durable_lockout_ack") == 0) restart_lockout_waits_for_off_commit();
        else if (strcmp(argv[1], "zero_target_relay_failure") == 0) zero_target_requires_relay_off();
        else if (strcmp(argv[1], "boot_maintenance_save") == 0) boot_maintenance_waits_for_shutdown_save();
        else if (strcmp(argv[1], "deferred_relay_disable") == 0) deferred_relay_start_waits_for_pending_disable();
        else if (strcmp(argv[1], "boot_off_interval") == 0) fresh_enable_after_reboot_waits_five_seconds();
        else if (strcmp(argv[1], "monitor_recovery_cancel") == 0) stopping_monitor_after_reboot_keeps_heat_disabled();
        else if (strcmp(argv[1], "monitor_save_failure") == 0) failed_monitor_save_turns_off_the_relay();
        else if (strcmp(argv[1], "retry_reset") == 0) sensor_retry_success_resets_count_and_fifth_failure_latches();
        else if (strcmp(argv[1], "retry_stall") == 0) acquisition_stall_after_failed_read_still_shuts_down();
        else if (strcmp(argv[1], "cadence") == 0) conversion_wait_preserves_cadence_and_stop_response();
        else if (strcmp(argv[1], "diagnostics") == 0) runtime_records_transition_history();
        else if (strcmp(argv[1], "remote_deadlines") == 0) remote_heat_commands_respect_deadlines_and_confirm_state();
        else if (strcmp(argv[1], "remote_enable_failure") == 0) rejected_enable_stays_off_when_relay_recovers();
        else if (strcmp(argv[1], "remote_enable_cancel") == 0) cancelled_remote_enable_stays_off();
        else if (strcmp(argv[1], "remote_enable_expiry") == 0) expired_remote_enable_rolls_back();
        else if (strcmp(argv[1], "fault_save_power_loss") == 0) overtemperature_publishes_before_flash_and_survives_power_loss();
        else if (strcmp(argv[1], "remote_disable_contention") == 0) remote_disable_turns_off_during_mode_save();
        else if (strcmp(argv[1], "remote_monitor_deadline") == 0) remote_monitor_stop_expires_at_runtime_lock();
        else if (strcmp(argv[1], "remote_monitor_cancel") == 0) interrupted_remote_monitor_stop_keeps_hold(false);
        else if (strcmp(argv[1], "remote_monitor_expiry") == 0) interrupted_remote_monitor_stop_keeps_hold(true);
        else if (strcmp(argv[1], "monitor_save_expiry") == 0) stalled_monitor_save_respects_permit(false);
        else if (strcmp(argv[1], "monitor_save_pause") == 0) stalled_monitor_save_respects_permit(true);
        else if (strncmp(argv[1], "ack_",4) == 0) acknowledgement_reports_controller_decision(argv[1]);
        else if (strcmp(argv[1], "calibration") == 0) calibration_applies_live_and_survives_restart();
        else if (strcmp(argv[1], "calibration_overtemp") == 0) calibration_enforces_overtemperature_without_new_sensor_sample();
        else if (strcmp(argv[1], "save_disable") == 0) blocked_save_does_not_delay_disable();
        else if (strcmp(argv[1], "save_sensor") == 0) blocked_save_does_not_delay_sensor_fault();
        else if (strcmp(argv[1], "save_overtemp") == 0) blocked_save_does_not_delay_overtemperature();
        else if (strcmp(argv[1], "save_stale") == 0) blocked_save_does_not_delay_stale_shutdown();
        else if (strcmp(argv[1], "save_queue") == 0) blocked_save_reserves_its_command();
        else if (strcmp(argv[1], "save_failure") == 0) failed_save_releases_reservation_without_changing_target();
        else if (strcmp(argv[1], "save_maintenance") == 0) maintenance_waits_for_an_admitted_save_without_blocking_outputs();
        else if (strcmp(argv[1], "monitor_requires") == 0) monitor_requires_enabled_heating();
        else if (strcmp(argv[1], "monitor_permit") == 0) monitor_expired_permit_holds_heater();
        else if (strcmp(argv[1], "monitor_disable") == 0) monitor_disable_then_enable();
        else if (strcmp(argv[1], "monitor_disable_enable") == 0) monitor_disable_and_enable_in_one_iteration();
        else if (strcmp(argv[1], "monitor_end") == 0) monitor_end_choices();
        else if (strcmp(argv[1], "monitor_maintenance") == 0) monitor_maintenance_ends();
        else if (strcmp(argv[1], "monitor_stale_end") == 0) monitor_stale_end_preserves_manual_heating();
        else if (strcmp(argv[1], "monitor_pending_end") == 0) monitor_pending_end_preserves_later_enable();
        else if (strcmp(argv[1], "restart_setting") == 0) restart_setting_uses_the_queued_target();
        else assert(!"unknown runtime persistence scenario");
        puts("runtime persistence safety regression passed");
        return 0;
    }
    fprintf(stderr,"urgent queue test\n");
    urgent_disable_discards_previously_queued_enables();
    fprintf(stderr,"existing maintenance tests\n");
    stalled_acquisition_shuts_down_and_requires_three_fresh_samples();
    conversion_wait_preserves_cadence_and_stop_response();
    /* The boot gate blocks commands before the first controller iteration. */
    assert(app_runtime_set_boot_maintenance(true) == ESP_OK);
    start(); check_rejected();
    assert(app_runtime_begin_maintenance(1000) == ESP_OK);
    assert(app_runtime_end_maintenance() == ESP_OK);
    enable();
    const uint64_t before = now_ms();
    assert(app_runtime_begin_maintenance(15000) == ESP_OK);
    assert(now_ms() - before < 1000);
    assert(!atomic_load(&heater));
    check_rejected();
    assert(app_runtime_end_maintenance() == ESP_OK);
    vTaskDelay(pdMS_TO_TICKS(500));
    app_status_t status; app_runtime_get_status(&status);
    assert(!status.enabled && !status.heater_on && !status.fan_on);

    /* An admitted NVS save reserves a command slot. A timeout must still
     * latch inhibition, and the admitted target must settle coherently. */
    enable();
    save_entered = xSemaphoreCreateBinary(); save_release = xSemaphoreCreateBinary();
    setter_done = xSemaphoreCreateBinary(); atomic_store(&block_save, true);
    assert(xTaskCreate(blocked_setter, "save", 4096, NULL, 5, NULL) == pdPASS);
    assert(xSemaphoreTake(save_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(app_runtime_begin_maintenance(100) == ESP_ERR_TIMEOUT);
    assert(app_runtime_maintenance_active());
    xSemaphoreGive(save_release);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(setter_result == ESP_OK);
    assert(app_runtime_begin_maintenance(15000) == ESP_OK);
    app_runtime_get_status(&status);
    assert(status.target_c == 55.0f && !status.enabled);
    check_rejected();
    assert(app_runtime_end_maintenance() == ESP_OK);
    atomic_store(&block_save, false);

    /* A failed output write cannot acknowledge maintenance or clear it. */
    enable(); atomic_store(&fail_output, true);
    assert(app_runtime_begin_maintenance(3000) == ESP_ERR_TIMEOUT);
    assert(app_runtime_maintenance_active() && atomic_load(&heater));
    assert(app_runtime_end_maintenance() == ESP_ERR_NOT_ALLOWED);
    atomic_store(&fail_output, false);
    assert(app_runtime_begin_maintenance(15000) == ESP_OK);
    assert(app_runtime_end_maintenance() == ESP_OK);
    app_runtime_stop();
    fault_survives_maintenance(HEAT_FAULT_SENSOR_OPEN);
    fault_survives_maintenance(HEAT_FAULT_NVS);
    fault_survives_maintenance(HEAT_FAULT_OUTPUT_INIT);
    puts("runtime maintenance: threaded admission, relay shutdown, timeout and failed-driver checks passed");
    return 0;
}
