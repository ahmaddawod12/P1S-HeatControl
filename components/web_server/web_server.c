#include "web_server.h"

#include <stdlib.h>
#include <string.h>

#include "api_contract.h"
#include "app_runtime.h"
#include "cJSON.h"
#include "diagnostics.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "http_request.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sse_hub.h"
#include "firmware_api.h"
#include "wifi_api.h"
#include "firmware_update.h"
#include "printer_api.h"
#include "printer_control.h"
#include "service_operation.h"

#define DEFAULT_PORT 80
#define MAX_OPEN_SOCKETS 7
#define BROADCAST_PERIOD_MS 1000U
#define KEEPALIVE_PERIOD_MS 15000U
#define BROADCAST_TASK_STACK 4096
#define BROADCAST_TASK_PRIORITY 4

static const char *TAG = "web";

static const char k_keepalive[] = ": keepalive\n\n";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char styles_css_start[] asm("_binary_styles_css_start");
extern const char styles_css_end[] asm("_binary_styles_css_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");
extern const char wifi_js_start[] asm("_binary_wifi_js_start");
extern const char wifi_js_end[] asm("_binary_wifi_js_end");
extern const char printer_js_start[] asm("_binary_printer_js_start");
extern const char printer_js_end[] asm("_binary_printer_js_end");

static httpd_handle_t s_server;
static web_server_ops_t s_ops;
static TaskHandle_t s_broadcast_task;
static volatile bool s_broadcasting;

static esp_err_t send_json(httpd_req_t *req,
                           const char *status_line,
                           const char *json)
{
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req,
                            const char *status_line,
                            const char *code,
                            const char *message)
{
    char json[160];
    if (api_error_json(code, message, json, sizeof(json)) != ESP_OK) {
        httpd_resp_set_status(req, status_line);
        return httpd_resp_sendstr(req, "{\"error\":\"internal\"}");
    }
    return send_json(req, status_line, json);
}

/* A rejected/incomplete request may still have unread bytes. Returning failure
 * makes IDF close it instead of purging a body that can keep trickling forever. */
static esp_err_t close_with_error(httpd_req_t *req, const char *status_line,
                                  const char *code, const char *message)
{
    char json[160];
    if (api_error_json(code, message, json, sizeof(json)) != ESP_OK)
        return http_request_close_json(req, status_line, "{\"error\":\"internal\"}");
    return http_request_close_json(req, status_line, json);
}

static bool chamber_request_trusted(httpd_req_t *req, bool allow_body)
{
    if (!firmware_api_request_trusted(req)) {
        (void) close_with_error(req, "403 Forbidden", "request_origin",
                            "a matching Origin and CSRF token are required");
        return false;
    }
    if (!allow_body && req->content_len != 0U) {
        (void) close_with_error(req, "400 Bad Request", "invalid_body",
                                "this command requires an empty body");
        return false;
    }
    return true;
}

static esp_err_t send_no_content(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* Maps a command result onto the documented status codes. */
static esp_err_t send_command_result(httpd_req_t *req, esp_err_t result)
{
    switch (result) {
    case ESP_OK:
        return send_no_content(req);
    case ESP_ERR_INVALID_ARG:
        return send_error(req, "400 Bad Request", "invalid_argument",
                          "the request was rejected by validation");
    case ESP_ERR_NOT_ALLOWED:
        if(service_operation_current()==SERVICE_OPERATION_PRINTER_CYCLE)
            return send_error(req,"409 Conflict","automatic_control","printer chamber control owns heating");
        return send_error(req, "409 Conflict", "maintenance", "firmware maintenance is active");
    case ESP_ERR_INVALID_STATE:
    case ESP_ERR_TIMEOUT:
        return send_error(req, "503 Service Unavailable", "unavailable",
                          "the controller is not accepting commands");
    default:
        return send_error(req, "500 Internal Server Error", "internal",
                          esp_err_to_name(result));
    }
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!http_request_require_empty_body(req)) return ESP_FAIL;
    app_status_t status;
    s_ops.get_status(&status);

    char json[API_STATUS_JSON_BYTES];
    if (api_status_json(&status, json, sizeof(json)) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "internal",
                          "status could not be serialized");
    }
    return send_json(req, "200 OK", json);
}

static esp_err_t diagnostics_handler(httpd_req_t *req)
{
    if (!http_request_require_empty_body(req)) return ESP_FAIL;
    // Keep the ring and JSON off the HTTP task's stack.
    typedef struct {
        diagnostic_record_t records[DIAGNOSTICS_CAPACITY];
        char json[API_DIAGNOSTICS_JSON_BYTES];
    } response_t;
    response_t *response = malloc(sizeof(*response));
    if (response == NULL) {
        return send_error(req, "500 Internal Server Error", "internal",
                          "diagnostics buffer could not be allocated");
    }
    size_t count = 0U;
    diagnostics_snapshot(response->records, DIAGNOSTICS_CAPACITY, &count);

    esp_err_t err = api_diagnostics_json(response->records, count,
                                         response->json, sizeof(response->json));
    if (err == ESP_OK) {
        err = send_json(req, "200 OK", response->json);
    } else {
        err = send_error(req, "500 Internal Server Error", "internal",
                         "diagnostics could not be serialized");
    }
    free(response);
    return err;
}

static esp_err_t setting_handler(httpd_req_t *req, bool target)
{
    if (!chamber_request_trusted(req, true)) return ESP_FAIL;
    char body[API_MAX_BODY_BYTES + 1U];
    const esp_err_t read_err = http_request_receive_body(req, body, sizeof(body));
    if (read_err == ESP_ERR_TIMEOUT) {
        return close_with_error(req, "408 Request Timeout", "body_timeout",
                          "the request body did not arrive in time");
    }
    if (read_err == ESP_ERR_INVALID_SIZE) {
        return close_with_error(req, "400 Bad Request", "body_too_large",
                          "the request body exceeds 256 bytes");
    }
    if (read_err != ESP_OK) {
        return close_with_error(req, "400 Bad Request", "unreadable_body",
                          "the request body could not be read");
    }

    float value = 0.0f;
    const esp_err_t parse_err = target ? api_parse_target(body, &value) : api_parse_calibration(body, &value);
    if (parse_err != ESP_OK) {
        return send_error(req, "400 Bad Request", target ? "invalid_target" : "invalid_calibration",
                          target ? "target_c must be 0 to 80 in 0.5 degree steps" :
                          "calibration_offset_c must be a number between -10 and 10");
    }
    return send_command_result(req, target ? s_ops.set_target(value) : s_ops.set_calibration(value));
}

static esp_err_t target_handler(httpd_req_t *req)
{
    return setting_handler(req, true);
}

static esp_err_t calibration_handler(httpd_req_t *req)
{
    return setting_handler(req, false);
}

/* The controller is authoritative; this precheck exists so the browser gets
 * a 409 instead of a silently ignored command. */
static const char *enable_rejection(const app_status_t *status)
{
    if (status->state == HEAT_STATE_FAULT) {
        return "a latched fault must be acknowledged first";
    }
    if (status->target_c <= 0.0f) {
        return "the target is zero, which disables heating";
    }
    if (!status->temperature.valid || status->temperature.consecutive_valid < 3U) {
        return "three consecutive valid samples are required";
    }
    return NULL;
}

static esp_err_t enable_handler(httpd_req_t *req)
{
    if (!chamber_request_trusted(req, false)) return ESP_FAIL;
    app_status_t status;
    s_ops.get_status(&status);

    const char *rejection = enable_rejection(&status);
    if (rejection != NULL) {
        return send_error(req, "409 Conflict", "enable_refused", rejection);
    }
    return send_command_result(req, s_ops.set_enabled(true));
}

static esp_err_t disable_handler(httpd_req_t *req)
{
    if (!chamber_request_trusted(req, false)) return ESP_FAIL;
    const esp_err_t result=s_ops.set_enabled(false);
    if(s_ops.stop_printer_cycle)(void)s_ops.stop_printer_cycle();
    return send_command_result(req,result);
}

static esp_err_t manual_fan_enable_handler(httpd_req_t *req)
{
    if (!chamber_request_trusted(req, false)) return ESP_FAIL;
    return send_command_result(req, s_ops.set_manual_fan(true));
}

static esp_err_t manual_fan_disable_handler(httpd_req_t *req)
{
    if (!chamber_request_trusted(req, false)) return ESP_FAIL;
    return send_command_result(req, s_ops.set_manual_fan(false));
}

static esp_err_t acknowledge_handler(httpd_req_t *req)
{
    if (!chamber_request_trusted(req, false)) return ESP_FAIL;
    app_status_t status;
    s_ops.get_status(&status);

    if (status.state != HEAT_STATE_FAULT) {
        return send_error(req, "409 Conflict", "no_fault",
                          "no fault is currently latched");
    }
    const esp_err_t result = s_ops.ack_fault();
    if (result == ESP_ERR_NOT_ALLOWED) {
        return send_error(req, "409 Conflict", "acknowledge_refused",
                          "fault recovery requirements are not satisfied");
    }
    return send_command_result(req, result);
}

size_t web_server_asset_length(const char *start, const char *end)
{
    if (start == NULL || end == NULL || end <= start) {
        return 0U;
    }
    /* EMBED_TXTFILES appends a NUL terminator that is not part of the file. */
    return (size_t) (end - start) - 1U;
}

static esp_err_t send_asset(httpd_req_t *req,
                            const char *start,
                            const char *end,
                            const char *content_type)
{
    if (!http_request_require_empty_body(req)) return ESP_FAIL;
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start,
                           (ssize_t) web_server_asset_length(start, end));
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return send_asset(req, index_html_start, index_html_end,
                      "text/html; charset=utf-8");
}

static esp_err_t styles_handler(httpd_req_t *req)
{
    return send_asset(req, styles_css_start, styles_css_end,
                      "text/css; charset=utf-8");
}

static esp_err_t script_handler(httpd_req_t *req)
{
    return send_asset(req, app_js_start, app_js_end,
                      "text/javascript; charset=utf-8");
}

static esp_err_t wifi_script_handler(httpd_req_t *req)
{
    return send_asset(req, wifi_js_start, wifi_js_end, "text/javascript; charset=utf-8");
}
static esp_err_t printer_script_handler(httpd_req_t *req)
{
    return send_asset(req,printer_js_start,printer_js_end,"text/javascript; charset=utf-8");
}

static esp_err_t events_handler(httpd_req_t *req)
{
    if (!http_request_require_empty_body(req)) return ESP_FAIL;
    const esp_err_t err = sse_hub_add_client(req);
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "503 Service Unavailable", "too_many_streams",
                          "at most four event streams are supported");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "internal",
                          "the event stream could not be opened");
    }
    return ESP_OK;
}

/* Polls once a second: a changed status goes out immediately, and an
 * unchanged one gets a comment frame every fifteen seconds so proxies and
 * browsers keep the connection open. */
static void broadcast_task(void *arg)
{
    (void) arg;
    char frame[API_SSE_FRAME_BYTES];
    char last_json[API_STATUS_JSON_BYTES] = {0};
    uint32_t quiet_ms = 0U;

    while (s_broadcasting) {
        vTaskDelay(pdMS_TO_TICKS(BROADCAST_PERIOD_MS));
        if (sse_hub_client_count() == 0U) {
            quiet_ms = 0U;
            continue;
        }

        app_status_t status;
        s_ops.get_status(&status);

        /* uptime_ms alone must not count as a change. */
        app_status_t comparison = status;
        comparison.uptime_ms = 0U;
        char comparable[API_STATUS_JSON_BYTES];
        if (api_status_json(&comparison, comparable, sizeof(comparable)) != ESP_OK) {
            continue;
        }

        if (strcmp(comparable, last_json) != 0) {
            size_t length = 0U;
            if (api_status_event(&status, frame, sizeof(frame), &length) ==
                ESP_OK) {
                sse_hub_broadcast(frame, length);
            }
            memcpy(last_json, comparable, sizeof(last_json));
            quiet_ms = 0U;
            continue;
        }

        quiet_ms += BROADCAST_PERIOD_MS;
        if (quiet_ms >= KEEPALIVE_PERIOD_MS) {
            sse_hub_broadcast(k_keepalive, sizeof(k_keepalive) - 1U);
            quiet_ms = 0U;
        }
    }

    s_broadcast_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t register_route(const char *uri,
                                httpd_method_t method,
                                esp_err_t (*handler)(httpd_req_t *))
{
    const httpd_uri_t route = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(s_server, &route);
}

web_server_config_t web_server_default_config(void)
{
    return (web_server_config_t) {
        .ops = {
            .get_status = app_runtime_get_status,
            .set_target = app_runtime_set_target,
            .set_calibration = app_runtime_set_calibration,
            .set_enabled = app_runtime_set_enabled,
            .set_manual_fan = app_runtime_set_manual_fan,
            .ack_fault = app_runtime_ack_fault,
            .get_firmware_status = firmware_update_get_status,
            .request_firmware_update = firmware_update_request,
            .get_wifi_status = wifi_manager_get_status,
            .request_wifi_change = wifi_manager_request_credentials,
            .get_printer_status = printer_control_get_status,
            .request_printer_config = printer_control_request_config,
            .forget_printer_config = printer_control_forget_config,
            .arm_printer_cycle = printer_control_arm,
            .stop_printer_cycle = printer_control_stop,
        },
        .port = DEFAULT_PORT,
    };
}

esp_err_t web_server_start(const web_server_config_t *config)
{
    if (config == NULL || config->ops.get_status == NULL ||
        config->ops.set_target == NULL || config->ops.set_calibration == NULL || config->ops.set_enabled == NULL ||
        config->ops.set_manual_fan == NULL ||
        config->ops.ack_fault == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ops = config->ops;

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = config->port;
    httpd_config.max_open_sockets = MAX_OPEN_SOCKETS;
    httpd_config.max_uri_handlers = 28;
    httpd_config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &httpd_config);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    static const struct {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    } k_routes[] = {
        {"/api/status", HTTP_GET, status_handler},
        {"/api/diagnostics", HTTP_GET, diagnostics_handler},
        {"/api/target", HTTP_PUT, target_handler},
        {"/api/calibration", HTTP_PUT, calibration_handler},
        {"/api/control/enable", HTTP_POST, enable_handler},
        {"/api/control/disable", HTTP_POST, disable_handler},
        {"/api/control/fan/enable", HTTP_POST, manual_fan_enable_handler},
        {"/api/control/fan/disable", HTTP_POST, manual_fan_disable_handler},
        {"/api/fault/acknowledge", HTTP_POST, acknowledge_handler},
        {"/api/events", HTTP_GET, events_handler},
        {"/", HTTP_GET, index_handler},
        {"/styles.css", HTTP_GET, styles_handler},
        {"/app.js", HTTP_GET, script_handler},
        {"/wifi.js", HTTP_GET, wifi_script_handler},
        {"/printer.js", HTTP_GET, printer_script_handler},
    };

    for (size_t i = 0U; i < sizeof(k_routes) / sizeof(k_routes[0]); ++i) {
        err = register_route(k_routes[i].uri, k_routes[i].method,
                             k_routes[i].handler);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "route %s failed: %s", k_routes[i].uri,
                     esp_err_to_name(err));
            (void) web_server_stop();
            return err;
        }
    }

    err = firmware_api_register(s_server, &s_ops);
    if (err != ESP_OK) { (void) web_server_stop(); return err; }
    err = wifi_api_register(s_server, &s_ops);
    if (err != ESP_OK) { (void) web_server_stop(); return err; }
    err = printer_api_register(s_server,&s_ops);
    if(err!=ESP_OK){(void)web_server_stop();return err;}
    s_broadcasting = true;
    if (xTaskCreate(broadcast_task, "sse_bcast", BROADCAST_TASK_STACK, NULL,
                    BROADCAST_TASK_PRIORITY, &s_broadcast_task) != pdPASS) {
        s_broadcasting = false;
        (void) web_server_stop();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening on port %u", (unsigned) config->port);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    s_broadcasting = false;
    while (s_broadcast_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    sse_hub_close_all();
    const esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
