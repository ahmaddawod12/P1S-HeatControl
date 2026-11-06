#include "api_contract.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "heat_controller.h"
#include "settings.h"

const char *api_state_name(heat_state_t state)
{
    switch (state) {
    case HEAT_STATE_BOOT:
        return "boot";
    case HEAT_STATE_IDLE:
        return "idle";
    case HEAT_STATE_HEATING:
        return "heating";
    case HEAT_STATE_HOLDING:
        return "holding";
    case HEAT_STATE_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

const char *api_fault_name(heat_fault_t fault)
{
    switch (fault) {
    case HEAT_FAULT_NONE:
        return "none";
    case HEAT_FAULT_SENSOR_OPEN:
        return "sensor_open";
    case HEAT_FAULT_SENSOR_SHORT:
        return "sensor_short";
    case HEAT_FAULT_SENSOR_RANGE:
        return "sensor_range";
    case HEAT_FAULT_SENSOR_INVALID:
        return "sensor_invalid";
    case HEAT_FAULT_SENSOR_STALE:
        return "sensor_stale";
    case HEAT_FAULT_OVERTEMP:
        return "overtemperature";
    case HEAT_FAULT_NVS:
        return "nvs";
    case HEAT_FAULT_OUTPUT_INIT:
        return "output_init";
    default:
        return "unknown";
    }
}

/* cJSON prints NaN and infinities as `null`; make that explicit so a bad
 * reading never reaches the browser as a number. */
static void add_number_or_null(cJSON *object, const char *name, float value)
{
    if (isfinite(value)) {
        cJSON_AddNumberToObject(object, name, (double) value);
    } else {
        cJSON_AddNullToObject(object, name);
    }
}

static esp_err_t print_into(cJSON *root, char *out_json, size_t capacity)
{
    char *text = cJSON_PrintUnformatted(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t length = strlen(text);
    esp_err_t err = ESP_OK;
    if (length + 1U > capacity) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        memcpy(out_json, text, length + 1U);
    }
    cJSON_free(text);
    return err;
}

/* Reject recursive values before cJSON can consume the small HTTP task stack.
 * These settings contain a single scalar field, so nesting is never useful. */
static bool setting_body_flat(const char *body, size_t length)
{
    bool quoted = false;
    unsigned depth = 0U;
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char c = (unsigned char) body[i];
        if (quoted && c == '\\') {
            if (i + 5U < length && body[i + 1U] == 'u' &&
                memcmp(body + i + 2U, "0000", 4U) == 0) {
                return false;
            }
            if (++i >= length) return false;
        } else if (c == '"') {
            quoted = !quoted;
        } else if (quoted) {
            if (c < 0x20U) return false;
        } else if (c == '{' || c == '[') {
            if (++depth > 1U) return false;
        } else if (c == '}' || c == ']') {
            if (depth == 0U) return false;
            --depth;
        }
    }
    return !quoted && depth == 0U;
}

static esp_err_t parse_setting(const char *body, float *out_value, bool target)
{
    if (body == NULL || out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strnlen(body, API_MAX_BODY_BYTES + 1U);
    if (length > API_MAX_BODY_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!setting_body_flat(body, length)) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsObject(root) && cJSON_GetArraySize(root) == 1) {
        const cJSON *number = cJSON_GetObjectItemCaseSensitive(
            root, target ? "target_c" : "calibration_offset_c");
        if (cJSON_IsNumber(number)) {
            const double value = number->valuedouble;
            const bool valid = target ? heat_target_is_valid((float) value) :
                (isfinite(value) && value >= SETTINGS_MIN_CALIBRATION_C &&
                 value <= SETTINGS_MAX_CALIBRATION_C);
            if (valid) {
                *out_value = (float) value;
                err = ESP_OK;
            }
        }
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t api_parse_target(const char *body, float *out_target)
{
    return parse_setting(body, out_target, true);
}

esp_err_t api_parse_calibration(const char *body, float *out_offset)
{
    return parse_setting(body, out_offset, false);
}

esp_err_t api_status_json(const app_status_t *status,
                          char *out_json,
                          size_t capacity)
{
    if (status == NULL || out_json == NULL || capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "state", api_state_name(status->state));
    cJSON_AddStringToObject(root, "fault", api_fault_name(status->fault));
    cJSON_AddBoolToObject(root, "enabled", status->enabled);
    cJSON_AddBoolToObject(root, "heater_on", status->heater_on);
    cJSON_AddBoolToObject(root, "fan_on", status->fan_on);
    cJSON_AddBoolToObject(root, "manual_fan_on", status->manual_fan_on);
    add_number_or_null(root, "target_c", status->target_c);
    add_number_or_null(root, "calibration_offset_c", status->calibration_offset_c);
    add_number_or_null(root, "raw_c", status->temperature.raw_c);
    add_number_or_null(root, "filtered_c", status->temperature.filtered_c);
    cJSON_AddBoolToObject(root, "valid", status->temperature.valid);
    cJSON_AddNumberToObject(root, "consecutive_valid",
                            status->temperature.consecutive_valid);
    cJSON_AddStringToObject(root, "temperature_fault",
                            api_fault_name(status->temperature.fault));
    cJSON_AddNumberToObject(root, "wifi_rssi", status->wifi_rssi);
    cJSON_AddStringToObject(root, "ip_addr", status->ip_addr);
    cJSON_AddNumberToObject(root, "uptime_ms", (double) status->uptime_ms);

    const esp_err_t err = print_into(root, out_json, capacity);
    cJSON_Delete(root);
    return err;
}

esp_err_t api_status_event(const app_status_t *status,
                           char *out_frame,
                           size_t capacity,
                           size_t *out_length)
{
    static const char k_prefix[] = "event: status\ndata: ";
    static const char k_suffix[] = "\n\n";
    const size_t prefix_length = sizeof(k_prefix) - 1U;
    const size_t suffix_length = sizeof(k_suffix) - 1U;

    if (out_frame == NULL || out_length == NULL ||
        capacity <= prefix_length + suffix_length) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(out_frame, k_prefix, prefix_length);
    const esp_err_t err = api_status_json(
        status, &out_frame[prefix_length],
        capacity - prefix_length - suffix_length);
    if (err != ESP_OK) {
        return err;
    }

    const size_t json_length = strlen(&out_frame[prefix_length]);
    memcpy(&out_frame[prefix_length + json_length], k_suffix,
           suffix_length + 1U);
    *out_length = prefix_length + json_length + suffix_length;
    return ESP_OK;
}

esp_err_t api_diagnostics_json(const diagnostic_record_t *records,
                               size_t count,
                               char *out_json,
                               size_t capacity)
{
    if ((records == NULL && count > 0U) || out_json == NULL || capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *array = cJSON_AddArrayToObject(root, "records");
    if (array == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0U; i < count; ++i) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(entry, "timestamp_ms",
                                (double) records[i].timestamp_ms);
        cJSON_AddStringToObject(entry, "from",
                                api_state_name(records[i].from_state));
        cJSON_AddStringToObject(entry, "to",
                                api_state_name(records[i].to_state));
        cJSON_AddStringToObject(entry, "fault",
                                api_fault_name(records[i].fault));
        cJSON_AddItemToArray(array, entry);
    }

    const esp_err_t err = print_into(root, out_json, capacity);
    cJSON_Delete(root);
    return err;
}

esp_err_t api_error_json(const char *code,
                         const char *message,
                         char *out_json,
                         size_t capacity)
{
    if (code == NULL || message == NULL || out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "error", code);
    cJSON_AddStringToObject(root, "message", message);

    const esp_err_t err = print_into(root, out_json, capacity);
    cJSON_Delete(root);
    return err;
}
