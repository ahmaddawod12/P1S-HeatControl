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

static atomic_bool heater, fan, fail_output, block_save;
static atomic_uint saves;
static atomic_int sensor_fault;
static _Atomic float sensor_temperature_c = 25.0f;
static atomic_int save_result = ESP_OK;
static atomic_bool slow_sensor, late_sample;
static atomic_uint slow_reads;
static atomic_ullong last_slow_read_ms;
static atomic_bool stall_sensor, stall_entered, release_stall;
static SemaphoreHandle_t save_entered, save_release, setter_done;
static esp_err_t setter_result;
static _Atomic float saved_calibration;
static atomic_uint calibration_saves;

void settings_factory_defaults(settings_data_t *data)
{ *data = (settings_data_t) {SETTINGS_SCHEMA_VERSION, 60.0f, 0.0f}; }
esp_err_t settings_load(settings_data_t *data, settings_load_result_t *result)
{ settings_factory_defaults(data); data->calibration_offset_c = atomic_load(&saved_calibration);
  *result = SETTINGS_LOAD_LOADED; return ESP_OK; }
esp_err_t settings_set_target(float value) { (void) value; return ESP_OK; }
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
        if (previous_ms && started_ms - previous_ms > 650U) atomic_store(&late_sample, true);
        atomic_fetch_add(&slow_reads, 1);
        vTaskDelay(pdMS_TO_TICKS(400)); /* Real DS18B20 driver conversion wait. */
    }
    *reading = (temperature_sensor_reading_t) {.temperature_c = atomic_load(&sensor_temperature_c),
        .fault = (temperature_sensor_fault_t) atomic_load(&sensor_fault)};
    return reading->fault == TEMPERATURE_SENSOR_FAULT_OPEN ? ESP_ERR_NOT_FOUND : ESP_OK;
}
esp_err_t board_outputs_init(const board_outputs_config_t *config)
{ (void) config; return ESP_OK; }
esp_err_t board_outputs_set(bool heat, bool cool)
{
    if (atomic_load(&fail_output)) return ESP_FAIL;
    atomic_store(&heater, heat); atomic_store(&fan, cool); return ESP_OK;
}
static esp_err_t read_sensor(temperature_sensor_reading_t *out, void *ctx)
{ (void) ctx; return temperature_sensor_read(NULL, out); }
static esp_err_t set_outputs(bool heat, bool cool, void *ctx)
{ (void) ctx; return board_outputs_set(heat, cool); }
static esp_err_t save_target(float value, void *ctx)
{
    (void) value; (void) ctx;
    if (atomic_load(&block_save)) {
        xSemaphoreGive(save_entered);
        assert(xSemaphoreTake(save_release, portMAX_DELAY) == pdTRUE);
    }
    atomic_fetch_add(&saves, 1);
    return atomic_load(&save_result);
}
static esp_err_t save_calibration(float value, void *ctx)
{ (void) ctx; return settings_set_calibration(value); }
static uint64_t now_ms(void) { return (uint64_t) esp_timer_get_time() / 1000; }
static app_status_t wait_status(bool ready, bool heat, uint32_t timeout)
{
    const uint64_t start = now_ms();
    app_status_t status;
    do {
        app_runtime_get_status(&status);
        if ((!ready || status.temperature.consecutive_valid >= 3) && status.heater_on == heat) return status;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (now_ms() - start < timeout);
    runtime_auto_status_t a;app_runtime_get_auto_status(&a);
    fprintf(stderr,"wait wanted heat=%d ready=%d: state=%d enabled=%d heat=%d fault=%d temp=%.2f streak=%u auto=%d id=%llu ended=%llu\n",heat,ready,status.state,status.enabled,status.heater_on,status.fault,status.temperature.filtered_c,status.temperature.consecutive_valid,a.active,(unsigned long long)a.cycle_id,(unsigned long long)a.last_ended_id);
    assert(!"controller did not reach expected state");
    return status;
}
static void start(void)
{
    const app_runtime_ops_t ops = {read_sensor, set_outputs, save_target, save_calibration, NULL};
    assert(app_runtime_start_with_ops(&ops) == ESP_OK);
    (void) wait_status(true, false, 4000);
}
static void enable(void)
{
    assert(app_runtime_set_enabled(true) == ESP_OK);
    (void) wait_status(true, true, 7000);
    assert(atomic_load(&heater));
}
static void automatic_lease_expires_without_producer(void)
{
    start();
    assert(app_runtime_auto_acquire(1,60.0f,now_ms()+5000)==ESP_OK);
    (void)wait_status(true,true,2000);
    assert(app_runtime_set_target(55.0f)==ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_set_enabled(true)==ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_set_manual_fan(true)==ESP_OK);
    (void)wait_status(true,false,6000);
    runtime_auto_status_t a;app_runtime_get_auto_status(&a);
    assert(!a.active&&a.teardown_applied&&a.last_ended_id==1);
    assert(app_runtime_auto_renew(1,now_ms()+5000)==ESP_ERR_INVALID_STATE);
    assert(app_runtime_auto_acquire(1,60.0f,now_ms()+5000)==ESP_ERR_INVALID_STATE);
    assert(app_runtime_auto_acquire(2,60.0f,now_ms()+5000)==ESP_OK);
    (void)wait_status(true,true,2000);
    for(unsigned i=0;i<100;i++)(void)app_runtime_set_manual_fan(true);
    assert(app_runtime_set_enabled(false)==ESP_OK);
    (void)wait_status(true,false,2000);
    app_runtime_get_auto_status(&a);assert(!a.active&&a.teardown_applied);
    app_runtime_stop();
}
static void urgent_disable_discards_previously_queued_enables(void)
{
    start();
    for(unsigned round=0;round<20;round++) {
        enable();
        for(unsigned i=0;i<32;i++) (void)app_runtime_set_enabled(true);
        if(round&1) app_runtime_auto_revoke(0);
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
    assert(app_runtime_set_manual_fan(true) == ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_ack_fault() == ESP_ERR_NOT_ALLOWED);
    assert(atomic_load(&saves) == count);
    assert(app_runtime_set_enabled(false) == ESP_OK);
}
static void blocked_setter(void *arg)
{ (void) arg; setter_result = app_runtime_set_target(55.0f); xSemaphoreGive(setter_done); vTaskDelete(NULL); }

static void begin_blocked_save(void)
{
    save_entered = xSemaphoreCreateBinary();
    save_release = xSemaphoreCreateBinary();
    setter_done = xSemaphoreCreateBinary();
    atomic_store(&block_save, true);
    assert(xTaskCreate(blocked_setter, "save", 4096, NULL, 5, NULL) == pdPASS);
    assert(xSemaphoreTake(save_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
}

static void finish_blocked_save(esp_err_t expected)
{
    assert(xSemaphoreTake(setter_done, 0) == pdFALSE);
    xSemaphoreGive(save_release);
    assert(xSemaphoreTake(setter_done, pdMS_TO_TICKS(1000)) == pdTRUE);
    assert(setter_result == expected);
    atomic_store(&block_save, false);
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
    assert(app_runtime_auto_acquire(77, 60.0f, now_ms() + 5000U) == ESP_OK);
    assert(app_runtime_set_calibration(1.0f) == ESP_ERR_NOT_ALLOWED);
    app_runtime_auto_revoke(77);
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
    const app_status_t status = wait_fault(HEAT_FAULT_SENSOR_OPEN, 1500);
    assert(!status.enabled && !status.fan_on && !atomic_load(&heater));
    finish_blocked_save(ESP_OK);
    atomic_store(&sensor_fault, TEMPERATURE_SENSOR_FAULT_NONE);
    app_runtime_stop();
}

static void blocked_save_does_not_delay_overtemperature(void)
{
    start(); enable(); begin_blocked_save();
    atomic_store(&sensor_temperature_c, 85.0f);
    const app_status_t status = wait_fault(HEAT_FAULT_OVERTEMP, 1500);
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

static void blocked_save_reserves_its_command_and_blocks_auto_acquisition(void)
{
    start(); begin_blocked_save();
    assert(app_runtime_auto_acquire(1, 60.0f, now_ms() + 5000) == ESP_ERR_NOT_ALLOWED);
    assert(app_runtime_set_target(56.0f) == ESP_ERR_TIMEOUT);
    for (unsigned i = 0; i < 64; ++i) (void) app_runtime_set_manual_fan((i & 1U) != 0);
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
    assert(app_runtime_auto_acquire(1, 60.0f, now_ms() + 5000) == ESP_OK);
    app_runtime_stop();
}

static void maintenance_waits_for_an_admitted_save_without_blocking_outputs(void)
{
    start(); begin_blocked_save();
    assert(app_runtime_begin_maintenance(500) == ESP_ERR_TIMEOUT);
    assert(app_runtime_maintenance_active() && !atomic_load(&heater) && !atomic_load(&fan));
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
    } while (now_ms() - began < 2000);
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
    atomic_store(&slow_sensor, true);
    start();
    assert(atomic_load(&slow_reads) >= 3);
    assert(!atomic_load(&late_sample));
    enable();
    const uint64_t began = now_ms();
    assert(app_runtime_set_enabled(false) == ESP_OK);
    (void) wait_status(false, false, 1000);
    assert(now_ms() - began < 400U);
    app_runtime_stop();
    atomic_store(&slow_sensor, false);
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
    (void)wait_fault(fault,3500);

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
    diagnostic_record_t records[DIAGNOSTICS_CAPACITY];size_t count=0;
    diagnostics_snapshot(records,DIAGNOSTICS_CAPACITY,&count);
    assert(count==1 && records[0].from_state==HEAT_STATE_BOOT && records[0].to_state==HEAT_STATE_IDLE);
    assert(records[0].fault==HEAT_FAULT_NONE && records[0].timestamp_ms>0);
    enable();
    atomic_store(&sensor_fault,TEMPERATURE_SENSOR_FAULT_OPEN);
    (void)wait_fault(HEAT_FAULT_SENSOR_OPEN,1500);
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

int main(int argc, char **argv)
{
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc == 2) {
        if (strcmp(argv[1], "diagnostics") == 0) runtime_records_transition_history();
        else if (strncmp(argv[1], "ack_",4) == 0) acknowledgement_reports_controller_decision(argv[1]);
        else if (strcmp(argv[1], "calibration") == 0) calibration_applies_live_and_survives_restart();
        else if (strcmp(argv[1], "calibration_overtemp") == 0) calibration_enforces_overtemperature_without_new_sensor_sample();
        else if (strcmp(argv[1], "save_disable") == 0) blocked_save_does_not_delay_disable();
        else if (strcmp(argv[1], "save_sensor") == 0) blocked_save_does_not_delay_sensor_fault();
        else if (strcmp(argv[1], "save_overtemp") == 0) blocked_save_does_not_delay_overtemperature();
        else if (strcmp(argv[1], "save_stale") == 0) blocked_save_does_not_delay_stale_shutdown();
        else if (strcmp(argv[1], "save_queue") == 0) blocked_save_reserves_its_command_and_blocks_auto_acquisition();
        else if (strcmp(argv[1], "save_failure") == 0) failed_save_releases_reservation_without_changing_target();
        else if (strcmp(argv[1], "save_maintenance") == 0) maintenance_waits_for_an_admitted_save_without_blocking_outputs();
        else assert(!"unknown runtime persistence scenario");
        puts("runtime persistence safety regression passed");
        return 0;
    }
    fprintf(stderr,"automatic lease test\n");
    automatic_lease_expires_without_producer();
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
    assert(now_ms() - before >= 10000); /* full cooldown, acknowledged fan off */
    assert(!atomic_load(&heater) && !atomic_load(&fan));
    check_rejected();
    assert(app_runtime_end_maintenance() == ESP_OK);
    vTaskDelay(pdMS_TO_TICKS(500));
    app_status_t status; app_runtime_get_status(&status);
    assert(!status.enabled && !status.heater_on && !status.manual_fan_on);

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
    puts("runtime maintenance: threaded admission, cooldown, timeout and failed-driver checks passed");
    return 0;
}
