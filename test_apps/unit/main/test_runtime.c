#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "app_runtime.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "unity.h"

#define WAIT_FOR(condition, timeout_ms)                                       \
    do {                                                                      \
        const int64_t deadline =                                              \
            esp_timer_get_time() + ((int64_t) (timeout_ms) * 1000);           \
        while (!(condition) && esp_timer_get_time() < deadline) {             \
            vTaskDelay(pdMS_TO_TICKS(10));                                    \
        }                                                                     \
        TEST_ASSERT_TRUE_MESSAGE((condition), "timed out: " #condition);      \
    } while (0)

typedef struct {
    SemaphoreHandle_t lock;
    float temperature_c;
    temperature_sensor_fault_t sensor_fault;
    esp_err_t read_result;
    int output_calls;
    bool heater_on;
    bool fan_on;
    int save_calls;
    float saved_target;
} fake_t;

static fake_t s_fake;
static bool s_fail_outputs;

static void fake_lock(void)
{
    xSemaphoreTake(s_fake.lock, portMAX_DELAY);
}

static void fake_unlock(void)
{
    xSemaphoreGive(s_fake.lock);
}

static esp_err_t fake_read_sensor(temperature_sensor_reading_t *out_reading, void *ctx)
{
    (void) ctx;
    fake_lock();
    const esp_err_t result = s_fake.read_result;
    *out_reading = (temperature_sensor_reading_t) {
        .temperature_c = s_fake.temperature_c,
        .fault = s_fake.sensor_fault,
    };
    fake_unlock();
    return result;
}

static esp_err_t fake_set_outputs(bool heater_on, bool fan_on, void *ctx)
{
    (void) ctx;
    if (s_fail_outputs) return ESP_FAIL;
    fake_lock();
    ++s_fake.output_calls;
    s_fake.heater_on = heater_on;
    s_fake.fan_on = fan_on;
    fake_unlock();
    return ESP_OK;
}

static esp_err_t fake_save_target(float target_c, void *ctx)
{
    (void) ctx;
    fake_lock();
    ++s_fake.save_calls;
    s_fake.saved_target = target_c;
    fake_unlock();
    return ESP_OK;
}

static const app_runtime_ops_t k_fake_ops = {
    .read_sensor = fake_read_sensor,
    .set_outputs = fake_set_outputs,
    .save_target = fake_save_target,
    .ctx = NULL,
};


static void fake_reset(float temperature_c, temperature_sensor_fault_t sensor_fault)
{
    s_fail_outputs = false;
    if (s_fake.lock == NULL) {
        s_fake.lock = xSemaphoreCreateMutex();
        TEST_ASSERT_NOT_NULL(s_fake.lock);
    }
    fake_lock();
    s_fake.temperature_c = temperature_c;
    s_fake.sensor_fault = sensor_fault;
    s_fake.read_result = ESP_OK;
    s_fake.output_calls = 0;
    s_fake.heater_on = false;
    s_fake.fan_on = false;
    s_fake.save_calls = 0;
    s_fake.saved_target = 0.0f;
    fake_unlock();
}

static void fake_set_sensor(float temperature_c, temperature_sensor_fault_t sensor_fault)
{
    fake_lock();
    s_fake.temperature_c = temperature_c;
    s_fake.sensor_fault = sensor_fault;
    fake_unlock();
}

static int fake_output_calls(void)
{
    fake_lock();
    const int calls = s_fake.output_calls;
    fake_unlock();
    return calls;
}

static bool fake_heater_on(void)
{
    fake_lock();
    const bool on = s_fake.heater_on;
    fake_unlock();
    return on;
}

static bool fake_fan_on(void)
{
    fake_lock();
    const bool on = s_fake.fan_on;
    fake_unlock();
    return on;
}

static void reset_nvs(void)
{
    nvs_flash_deinit();
    TEST_ESP_OK(nvs_flash_erase());
    TEST_ESP_OK(nvs_flash_init());
}

static app_status_t status_now(void)
{
    app_status_t status;
    app_runtime_get_status(&status);
    return status;
}

static bool state_is(heat_state_t state)
{
    return status_now().state == state;
}

static bool has_valid_samples(uint8_t count)
{
    const app_status_t status = status_now();
    return status.temperature.valid &&
           status.temperature.consecutive_valid >= count;
}

/* Brings the runtime up with a healthy sensor and waits for regulation-ready
 * samples so enable commands are accepted. */
static void start_ready_runtime(float temperature_c)
{
    reset_nvs();
    fake_reset(temperature_c, TEMPERATURE_SENSOR_FAULT_NONE);
    TEST_ESP_OK(app_runtime_start_with_ops(&k_fake_ops));
    WAIT_FOR(has_valid_samples(3), 5000);
}

TEST_CASE("runtime starts disabled with the stored target and outputs off",
          "[runtime]")
{
    reset_nvs();
    fake_reset(25.0f, TEMPERATURE_SENSOR_FAULT_NONE);

    TEST_ESP_OK(app_runtime_start_with_ops(&k_fake_ops));

    app_status_t status = status_now();
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_FALSE(status.fan_on);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, status.target_c);

    WAIT_FOR(state_is(HEAT_STATE_IDLE), 3000);
    status = status_now();
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_FALSE(status.fan_on);
    TEST_ASSERT_EQUAL_INT(1, fake_output_calls());
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_FALSE(fake_fan_on());

    app_runtime_stop();
}

TEST_CASE("automatic heating lease expires and never renews a consumed run", "[runtime]")
{
    start_ready_runtime(25.0f);
    const uint64_t now=(uint64_t)esp_timer_get_time()/1000;
    TEST_ESP_OK(app_runtime_auto_acquire(11,status_now().target_c,now+5000));
    WAIT_FOR(fake_heater_on(),1000);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED,app_runtime_set_target(55));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED,app_runtime_set_enabled(true));
    WAIT_FOR(!fake_heater_on(),6000);
    runtime_auto_status_t automatic;app_runtime_get_auto_status(&automatic);
    TEST_ASSERT_FALSE(automatic.active);TEST_ASSERT_TRUE(automatic.teardown_applied);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,app_runtime_auto_renew(11,(uint64_t)esp_timer_get_time()/1000+5000));
    app_runtime_stop();
}

TEST_CASE("manual fan runs independently while heating is disabled",
          "[runtime]")
{
    start_ready_runtime(25.0f);

    TEST_ESP_OK(app_runtime_set_manual_fan(true));
    WAIT_FOR(status_now().manual_fan_on && status_now().fan_on, 2000);
    app_status_t status = status_now();
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_TRUE(status.manual_fan_on);
    TEST_ASSERT_TRUE(status.fan_on);

    TEST_ESP_OK(app_runtime_set_manual_fan(false));
    WAIT_FOR(!status_now().manual_fan_on && !status_now().fan_on, 2000);
    status = status_now();
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_FALSE(status.manual_fan_on);
    TEST_ASSERT_FALSE(status.fan_on);

    app_runtime_stop();
}

TEST_CASE("heating overrides a manual fan off request", "[runtime]")
{
    start_ready_runtime(50.0f);
    TEST_ESP_OK(app_runtime_set_manual_fan(true));
    WAIT_FOR(status_now().manual_fan_on && status_now().fan_on, 2000);

    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(state_is(HEAT_STATE_HEATING), 2000);
    TEST_ESP_OK(app_runtime_set_manual_fan(false));
    WAIT_FOR(!status_now().manual_fan_on, 2000);

    const app_status_t status = status_now();
    TEST_ASSERT_TRUE(status.enabled);
    TEST_ASSERT_TRUE(status.heater_on);
    TEST_ASSERT_FALSE(status.manual_fan_on);
    TEST_ASSERT_TRUE(status.fan_on);

    app_runtime_stop();
}

TEST_CASE("target updates are validated, persisted, and published", "[runtime]")
{
    start_ready_runtime(25.0f);

    TEST_ESP_OK(app_runtime_set_target(70.5f));
    WAIT_FOR(status_now().target_c > 70.0f, 2000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 70.5f, status_now().target_c);

    fake_lock();
    TEST_ASSERT_EQUAL_INT(1, s_fake.save_calls);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 70.5f, s_fake.saved_target);
    fake_unlock();

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, app_runtime_set_target(70.2f));
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 70.5f, status_now().target_c);
    fake_lock();
    TEST_ASSERT_EQUAL_INT(1, s_fake.save_calls);
    fake_unlock();

    app_runtime_stop();
}

TEST_CASE("enabling below target heats with one paired-on call", "[runtime]")
{
    start_ready_runtime(50.0f);
    TEST_ASSERT_EQUAL_INT(1, fake_output_calls());

    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(state_is(HEAT_STATE_HEATING), 2000);

    const app_status_t status = status_now();
    TEST_ASSERT_TRUE(status.enabled);
    TEST_ASSERT_TRUE(status.heater_on);
    TEST_ASSERT_TRUE(status.fan_on);
    TEST_ASSERT_EQUAL_INT(2, fake_output_calls());
    TEST_ASSERT_TRUE(fake_heater_on());
    TEST_ASSERT_TRUE(fake_fan_on());

    app_runtime_stop();
}

TEST_CASE("enabling at target does not start fan cooldown", "[runtime]")
{
    start_ready_runtime(60.0f);

    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(state_is(HEAT_STATE_HOLDING), 2000);

    const app_status_t status = status_now();
    TEST_ASSERT_TRUE(status.enabled);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_FALSE(status.fan_on);
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_FALSE(fake_fan_on());

    app_runtime_stop();
}

TEST_CASE("reaching target starts fan cooldown", "[runtime]")
{
    start_ready_runtime(50.0f);
    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(state_is(HEAT_STATE_HEATING), 2000);

    fake_set_sensor(60.0f, TEMPERATURE_SENSOR_FAULT_NONE);
    WAIT_FOR(state_is(HEAT_STATE_HOLDING), 2000);

    const app_status_t status = status_now();
    TEST_ASSERT_TRUE(status.enabled);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_TRUE(status.fan_on);
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_TRUE(fake_fan_on());

    app_runtime_stop();
}

TEST_CASE("disable stops heater and keeps fan on for cooldown", "[runtime]")
{
    start_ready_runtime(50.0f);
    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(state_is(HEAT_STATE_HEATING), 2000);

    TEST_ESP_OK(app_runtime_set_enabled(false));
    WAIT_FOR(state_is(HEAT_STATE_IDLE), 2000);

    const app_status_t status = status_now();
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_TRUE(status.fan_on);
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_TRUE(fake_fan_on());

    app_runtime_stop();
}

TEST_CASE("target zero stops heater and keeps fan on for cooldown", "[runtime]")
{
    start_ready_runtime(50.0f);
    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(state_is(HEAT_STATE_HEATING), 2000);

    TEST_ESP_OK(app_runtime_set_target(0.0f));
    WAIT_FOR(state_is(HEAT_STATE_IDLE), 2000);

    const app_status_t status = status_now();
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, status.target_c);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_TRUE(status.fan_on);
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_TRUE(fake_fan_on());

    app_runtime_stop();
}

TEST_CASE("a sensor fault stops heater and fan immediately", "[runtime]")
{
    start_ready_runtime(50.0f);
    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(state_is(HEAT_STATE_HEATING), 2000);

    fake_set_sensor(50.0f, TEMPERATURE_SENSOR_FAULT_OPEN);
    WAIT_FOR(state_is(HEAT_STATE_FAULT), 3000);

    const app_status_t status = status_now();
    TEST_ASSERT_EQUAL(HEAT_FAULT_SENSOR_OPEN, status.fault);
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_FALSE(status.fan_on);
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_FALSE(fake_fan_on());
    TEST_ASSERT_EQUAL_INT(3, fake_output_calls());

    app_runtime_stop();
}

TEST_CASE("network status updates never disturb the controller", "[runtime]")
{
    start_ready_runtime(50.0f);
    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(state_is(HEAT_STATE_HEATING), 2000);
    const int calls_before = fake_output_calls();

    app_runtime_set_network_status(-55, "192.168.1.50");
    WAIT_FOR(status_now().wifi_rssi == -55, 2000);

    const app_status_t status = status_now();
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", status.ip_addr);
    TEST_ASSERT_EQUAL(HEAT_STATE_HEATING, status.state);
    TEST_ASSERT_TRUE(status.heater_on);
    TEST_ASSERT_TRUE(status.fan_on);
    TEST_ASSERT_EQUAL_INT(calls_before, fake_output_calls());

    app_runtime_stop();
}

TEST_CASE("a reported startup fault latches with outputs off", "[runtime]")
{
    start_ready_runtime(25.0f);

    TEST_ESP_OK(app_runtime_report_startup_fault(HEAT_FAULT_NVS));
    WAIT_FOR(state_is(HEAT_STATE_FAULT), 2000);
    TEST_ASSERT_EQUAL(HEAT_FAULT_NVS, status_now().fault);
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_FALSE(fake_fan_on());

    TEST_ESP_OK(app_runtime_set_enabled(true));
    vTaskDelay(pdMS_TO_TICKS(300));
    TEST_ASSERT_EQUAL(HEAT_STATE_FAULT, status_now().state);
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_FALSE(fake_fan_on());

    TEST_ESP_OK(app_runtime_ack_fault());
    WAIT_FOR(state_is(HEAT_STATE_IDLE), 2000);
    TEST_ASSERT_EQUAL(HEAT_FAULT_NONE, status_now().fault);
    TEST_ASSERT_FALSE(status_now().enabled);

    app_runtime_stop();
}

TEST_CASE("recovered settings corruption latches an NVS fault", "[runtime]")
{
    reset_nvs();

    nvs_handle_t handle;
    TEST_ESP_OK(nvs_open("heat_cfg", NVS_READWRITE, &handle));
    const uint8_t malformed[] = {0x01, 0x02, 0x03};
    TEST_ESP_OK(nvs_set_blob(handle, "settings", malformed, sizeof(malformed)));
    TEST_ESP_OK(nvs_commit(handle));
    nvs_close(handle);

    fake_reset(25.0f, TEMPERATURE_SENSOR_FAULT_NONE);
    TEST_ESP_OK(app_runtime_start_with_ops(&k_fake_ops));

    WAIT_FOR(state_is(HEAT_STATE_FAULT), 3000);
    const app_status_t status = status_now();
    TEST_ASSERT_EQUAL(HEAT_FAULT_NVS, status.fault);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, status.target_c);
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_FALSE(fake_fan_on());

    app_runtime_stop();
}

TEST_CASE("maintenance blocks queued enable and target persistence", "[runtime]")
{
    start_ready_runtime(25.0f);
    TEST_ESP_OK(app_runtime_set_enabled(true));
    TEST_ESP_OK(app_runtime_begin_maintenance(15000));
    TEST_ASSERT_TRUE(app_runtime_maintenance_active());
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_FALSE(fake_fan_on());
    const int saves = s_fake.save_calls;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, app_runtime_set_enabled(true));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, app_runtime_set_target(55.5f));
    TEST_ASSERT_EQUAL(saves, s_fake.save_calls);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, app_runtime_set_manual_fan(true));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, app_runtime_ack_fault());
    TEST_ESP_OK(app_runtime_set_enabled(false));
    TEST_ESP_OK(app_runtime_end_maintenance());
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_FALSE(fake_heater_on());
    app_runtime_stop();
}

TEST_CASE("maintenance clears manual fan after cooldown", "[runtime]")
{
    start_ready_runtime(25.0f);
    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(fake_heater_on(), 2000);
    TEST_ESP_OK(app_runtime_set_manual_fan(true));
    TEST_ESP_OK(app_runtime_begin_maintenance(15000));
    TEST_ASSERT_FALSE(fake_heater_on());
    TEST_ASSERT_FALSE(fake_fan_on());
    TEST_ASSERT_FALSE(status_now().manual_fan_on);
    TEST_ESP_OK(app_runtime_end_maintenance());
    app_runtime_stop();
}

TEST_CASE("maintenance timeout never acknowledges failed output writes", "[runtime]")
{
    start_ready_runtime(25.0f);
    TEST_ESP_OK(app_runtime_set_enabled(true));
    WAIT_FOR(fake_heater_on(), 2000);
    s_fail_outputs = true;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, app_runtime_begin_maintenance(200));
    TEST_ASSERT_TRUE(app_runtime_maintenance_active());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, app_runtime_end_maintenance());
    s_fail_outputs = false;
    app_runtime_stop();
}
