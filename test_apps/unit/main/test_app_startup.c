#include <stddef.h>
#include <string.h>

#include "app_startup.h"
#include "nvs.h"
#include "unity.h"

#define LOG_CAPACITY 12U
#define NAME_CAPACITY 32U

static char s_log[LOG_CAPACITY][NAME_CAPACITY];
static size_t s_count;

static esp_err_t s_board_result;
static esp_err_t s_nvs_result;
static esp_err_t s_settings_result;
static esp_err_t s_runtime_result;
static esp_err_t s_wifi_result;
static esp_err_t s_web_result;

static void note(const char *name)
{
    if (s_count < LOG_CAPACITY) {
        strncpy(s_log[s_count], name, NAME_CAPACITY - 1U);
        s_log[s_count][NAME_CAPACITY - 1U] = '\0';
    }
    ++s_count;
}

static esp_err_t fake_board_init(void)
{
    note("board_outputs_init");
    return s_board_result;
}

static esp_err_t fake_nvs_init(void)
{
    note("nvs_flash_init");
    return s_nvs_result;
}

static esp_err_t fake_settings_init(void)
{
    note("settings_init");
    return s_settings_result;
}

static esp_err_t fake_runtime_start(void)
{
    note("app_runtime_start");
    return s_runtime_result;
}

static esp_err_t fake_wifi_start(void)
{
    note("wifi_manager_start");
    return s_wifi_result;
}

static esp_err_t fake_web_start(void)
{
    note("web_server_start");
    return s_web_result;
}

static void fake_force_off(void)
{
    note("board_outputs_force_off");
}

static esp_err_t fake_report_fault(heat_fault_t fault)
{
    note(fault == HEAT_FAULT_OUTPUT_INIT ? "report_output_init_fault"
                                         : "report_fault");
    return ESP_OK;
}

static app_startup_ops_t fake_ops(void)
{
    s_count = 0U;
    memset(s_log, 0, sizeof(s_log));
    s_board_result = ESP_OK;
    s_nvs_result = ESP_OK;
    s_settings_result = ESP_OK;
    s_runtime_result = ESP_OK;
    s_wifi_result = ESP_OK;
    s_web_result = ESP_OK;

    return (app_startup_ops_t) {
        .board_outputs_init = fake_board_init,
        .nvs_flash_init = fake_nvs_init,
        .settings_init = fake_settings_init,
        .runtime_start = fake_runtime_start,
        .wifi_start = fake_wifi_start,
        .web_server_start = fake_web_start,
        .board_outputs_force_off = fake_force_off,
        .report_startup_fault = fake_report_fault,
    };
}

static void assert_sequence(const char *const *expected, size_t count)
{
    TEST_ASSERT_EQUAL_UINT32(count, s_count);
    for (size_t i = 0U; i < count; ++i) {
        TEST_ASSERT_EQUAL_STRING(expected[i], s_log[i]);
    }
}

TEST_CASE("startup brings services up in a safe order", "[startup]")
{
    app_startup_ops_t ops = fake_ops();

    TEST_ESP_OK(app_startup_run(&ops));

    static const char *const k_expected[] = {
        "board_outputs_init",
        "nvs_flash_init",
        "settings_init",
        "app_runtime_start",
        "wifi_manager_start",
        "web_server_start",
    };
    /* Exact equality also proves nothing enabled regulation on the way up. */
    assert_sequence(k_expected, sizeof(k_expected) / sizeof(k_expected[0]));
}

TEST_CASE("a relay init failure stops startup with outputs off", "[startup]")
{
    app_startup_ops_t ops = fake_ops();
    s_board_result = ESP_ERR_INVALID_STATE;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, app_startup_run(&ops));

    static const char *const k_expected[] = {
        "board_outputs_init",
        "board_outputs_force_off",
    };
    assert_sequence(k_expected, sizeof(k_expected) / sizeof(k_expected[0]));
}

TEST_CASE("a runtime failure stops before networking", "[startup]")
{
    app_startup_ops_t ops = fake_ops();
    s_runtime_result = ESP_ERR_NO_MEM;

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, app_startup_run(&ops));

    static const char *const k_expected[] = {
        "board_outputs_init",
        "nvs_flash_init",
        "settings_init",
        "app_runtime_start",
        "board_outputs_force_off",
    };
    assert_sequence(k_expected, sizeof(k_expected) / sizeof(k_expected[0]));
}

TEST_CASE("networking failure keeps the runtime alive", "[startup]")
{
    app_startup_ops_t ops = fake_ops();
    s_wifi_result = ESP_ERR_INVALID_STATE;

    /* Regulation survives; the caller retries networking. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, app_startup_run(&ops));

    static const char *const k_expected[] = {
        "board_outputs_init",
        "nvs_flash_init",
        "settings_init",
        "app_runtime_start",
        "wifi_manager_start",
        "board_outputs_force_off",
        "web_server_start",
    };
    assert_sequence(k_expected, sizeof(k_expected) / sizeof(k_expected[0]));
}

TEST_CASE("a web server failure forces outputs off", "[startup]")
{
    app_startup_ops_t ops = fake_ops();
    s_web_result = ESP_ERR_NO_MEM;

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, app_startup_run(&ops));

    static const char *const k_expected[] = {
        "board_outputs_init",
        "nvs_flash_init",
        "settings_init",
        "app_runtime_start",
        "wifi_manager_start",
        "web_server_start",
        "board_outputs_force_off",
    };
    assert_sequence(k_expected, sizeof(k_expected) / sizeof(k_expected[0]));
}

TEST_CASE("settings recovery never blocks startup", "[startup]")
{
    app_startup_ops_t ops = fake_ops();
    s_settings_result = ESP_ERR_NVS_NOT_FOUND;

    /* The runtime latches HEAT_FAULT_NVS itself; startup carries on. */
    TEST_ESP_OK(app_startup_run(&ops));

    static const char *const k_expected[] = {
        "board_outputs_init",
        "nvs_flash_init",
        "settings_init",
        "app_runtime_start",
        "wifi_manager_start",
        "web_server_start",
    };
    assert_sequence(k_expected, sizeof(k_expected) / sizeof(k_expected[0]));
}

TEST_CASE("startup rejects an incomplete operations table", "[startup]")
{
    app_startup_ops_t ops = fake_ops();
    ops.runtime_start = NULL;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, app_startup_run(&ops));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, app_startup_run(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, s_count);
}
