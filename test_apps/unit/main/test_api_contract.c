#include <stddef.h>
#include <string.h>

#include "api_contract.h"
#include "unity.h"

static app_status_t sample_status(void)
{
    app_status_t status = app_status_initial();
    status.state = HEAT_STATE_HEATING;
    status.fault = HEAT_FAULT_NONE;
    status.enabled = true;
    status.heater_on = true;
    status.fan_on = true;
    status.manual_fan_on = true;
    status.target_c = 62.5f;
    status.temperature = (temperature_sample_t) {
        .raw_c = 48.25f,
        .filtered_c = 48.5f,
        .valid = true,
        .consecutive_valid = 5U,
        .timestamp_ms = 123456U,
        .fault = HEAT_FAULT_NONE,
    };
    status.wifi_rssi = -47;
    strcpy(status.ip_addr, "192.168.1.42");
    status.uptime_ms = 987654U;
    return status;
}

TEST_CASE("valid target bodies parse to a float", "[api]")
{
    float target = 0.0f;

    TEST_ASSERT_EQUAL(ESP_OK, api_parse_target("{\"target_c\":60.0}", &target));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, target);

    TEST_ASSERT_EQUAL(ESP_OK, api_parse_target("{\"target_c\":0}", &target));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, target);

    TEST_ASSERT_EQUAL(ESP_OK, api_parse_target("{\"target_c\":80}", &target));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 80.0f, target);

    TEST_ASSERT_EQUAL(ESP_OK, api_parse_target("{\"target_c\":57.5}", &target));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 57.5f, target);
}

TEST_CASE("out-of-contract target bodies are rejected", "[api]")
{
    float target = 60.0f;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      api_parse_target("{\"target_c\":60.2}", &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      api_parse_target("{\"target_c\":81}", &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      api_parse_target("{\"target_c\":-0.5}", &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      api_parse_target("{\"target_c\":\"hot\"}", &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      api_parse_target("{\"target\":60.0}", &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, api_parse_target("{}", &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, api_parse_target("[60.0]", &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, api_parse_target("not json", &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, api_parse_target(NULL, &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      api_parse_target("{\"target_c\":60.0}", NULL));

    /* The caller's value survives a rejected body. */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, target);
}

TEST_CASE("oversized request bodies are rejected", "[api]")
{
    char body[API_MAX_BODY_BYTES + 32U];
    memset(body, ' ', sizeof(body));
    memcpy(body, "{\"target_c\":60.0}", strlen("{\"target_c\":60.0}"));
    body[sizeof(body) - 1U] = '\0';

    float target = 0.0f;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, api_parse_target(body, &target));
}

TEST_CASE("status serializes the full contract", "[api]")
{
    const app_status_t status = sample_status();
    char json[API_STATUS_JSON_BYTES];

    TEST_ESP_OK(api_status_json(&status, json, sizeof(json)));

    TEST_ASSERT_NOT_NULL(strstr(json, "\"state\":\"heating\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"fault\":\"none\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"enabled\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"heater_on\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"fan_on\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"manual_fan_on\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"target_c\":62.5"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"raw_c\":48.25"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"filtered_c\":48.5"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"valid\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"ip_addr\":\"192.168.1.42\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"wifi_rssi\":-47"));

    /* Quotes are numbers, not strings. */
    TEST_ASSERT_NULL(strstr(json, "\"target_c\":\""));
    TEST_ASSERT_NULL(strstr(json, "\"raw_c\":\""));
}

TEST_CASE("invalid readings serialize as null rather than NaN", "[api]")
{
    app_status_t status = sample_status();
    status.state = HEAT_STATE_FAULT;
    status.fault = HEAT_FAULT_SENSOR_STALE;
    status.enabled = false;
    status.heater_on = false;
    status.fan_on = false;
    status.temperature.valid = false;
    status.temperature.raw_c = (float) (0.0 / 0.0);
    status.temperature.filtered_c = (float) (0.0 / 0.0);

    char json[API_STATUS_JSON_BYTES];
    TEST_ESP_OK(api_status_json(&status, json, sizeof(json)));

    TEST_ASSERT_NOT_NULL(strstr(json, "\"state\":\"fault\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"fault\":\"sensor_stale\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"raw_c\":null"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"filtered_c\":null"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"enabled\":false"));
    TEST_ASSERT_NULL(strstr(json, "nan"));
    TEST_ASSERT_NULL(strstr(json, "NaN"));
}

TEST_CASE("state and fault names are stable", "[api]")
{
    TEST_ASSERT_EQUAL_STRING("boot", api_state_name(HEAT_STATE_BOOT));
    TEST_ASSERT_EQUAL_STRING("idle", api_state_name(HEAT_STATE_IDLE));
    TEST_ASSERT_EQUAL_STRING("heating", api_state_name(HEAT_STATE_HEATING));
    TEST_ASSERT_EQUAL_STRING("holding", api_state_name(HEAT_STATE_HOLDING));
    TEST_ASSERT_EQUAL_STRING("fault", api_state_name(HEAT_STATE_FAULT));

    TEST_ASSERT_EQUAL_STRING("none", api_fault_name(HEAT_FAULT_NONE));
    TEST_ASSERT_EQUAL_STRING("sensor_open",
                             api_fault_name(HEAT_FAULT_SENSOR_OPEN));
    TEST_ASSERT_EQUAL_STRING("sensor_short",
                             api_fault_name(HEAT_FAULT_SENSOR_SHORT));
    TEST_ASSERT_EQUAL_STRING("sensor_range",
                             api_fault_name(HEAT_FAULT_SENSOR_RANGE));
    TEST_ASSERT_EQUAL_STRING("sensor_invalid",
                             api_fault_name(HEAT_FAULT_SENSOR_INVALID));
    TEST_ASSERT_EQUAL_STRING("sensor_stale",
                             api_fault_name(HEAT_FAULT_SENSOR_STALE));
    TEST_ASSERT_EQUAL_STRING("overtemperature",
                             api_fault_name(HEAT_FAULT_OVERTEMP));
    TEST_ASSERT_EQUAL_STRING("nvs", api_fault_name(HEAT_FAULT_NVS));
    TEST_ASSERT_EQUAL_STRING("output_init",
                             api_fault_name(HEAT_FAULT_OUTPUT_INIT));
}

TEST_CASE("an sse frame carries one status event", "[api]")
{
    const app_status_t status = sample_status();
    char frame[API_SSE_FRAME_BYTES];
    size_t length = 0U;

    TEST_ESP_OK(api_status_event(&status, frame, sizeof(frame), &length));

    TEST_ASSERT_EQUAL_UINT32(strlen(frame), length);
    TEST_ASSERT_EQUAL_INT(0, strncmp(frame, "event: status\ndata: ", 20));
    TEST_ASSERT_EQUAL_INT(0, strcmp(&frame[length - 2U], "\n\n"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\"state\":\"heating\""));
    /* Exactly one blank-line terminator. */
    TEST_ASSERT_NULL(strstr(frame, "\n\n\n"));
}
