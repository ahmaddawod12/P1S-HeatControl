#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_contract.h"
#include "diagnostics.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sse_hub.h"
#include "unity.h"
#include "web_server.h"
#include "cJSON.h"

#define TEST_PORT 8180
#define RESPONSE_CAPACITY 2048

typedef struct {
    app_status_t status;
    int set_target_calls;
    float last_target;
    int set_enabled_calls;
    bool last_enabled;
    int set_manual_fan_calls;
    bool last_manual_fan;
    int ack_calls;
    esp_err_t command_result;
    int update_calls;
    firmware_status_t firmware;
    wifi_manager_status_t wifi;
    wifi_credentials_t requested_wifi;
    int wifi_calls;
} fake_service_t;

static fake_service_t s_service;

static void fake_get_status(app_status_t *out_status)
{
    *out_status = s_service.status;
}

static esp_err_t fake_set_target(float target_c)
{
    ++s_service.set_target_calls;
    s_service.last_target = target_c;
    return s_service.command_result;
}

static esp_err_t fake_set_enabled(bool enabled)
{
    ++s_service.set_enabled_calls;
    s_service.last_enabled = enabled;
    return s_service.command_result;
}

static esp_err_t fake_set_manual_fan(bool enabled)
{
    ++s_service.set_manual_fan_calls;
    s_service.last_manual_fan = enabled;
    return s_service.command_result;
}

static esp_err_t fake_ack_fault(void)
{
    ++s_service.ack_calls;
    return s_service.command_result;
}

static void fake_firmware_status(firmware_status_t *out) { *out = s_service.firmware; }
static esp_err_t fake_firmware_update(void) { ++s_service.update_calls; return s_service.command_result; }
static void fake_wifi_status(wifi_manager_status_t *out) { *out=s_service.wifi; }
static esp_err_t fake_wifi_change(const wifi_credentials_t *credentials)
{ ++s_service.wifi_calls; s_service.requested_wifi=*credentials; return s_service.command_result; }

/* A regulation-ready device: valid samples, idle, target 60. */
static void reset_service(void)
{
    memset(&s_service, 0, sizeof(s_service));
    s_service.command_result = ESP_OK;
    s_service.status = app_status_initial();
    s_service.status.state = HEAT_STATE_IDLE;
    s_service.status.target_c = 60.0f;
    strcpy(s_service.firmware.installed, "1.0.0");
    s_service.wifi=(wifi_manager_status_t){.configured=true,.connected=true,.can_save=true};
    strcpy(s_service.wifi.ssid,"fixture network");
    strcpy(s_service.wifi.password,"visible password");
    strcpy(s_service.wifi.ip_addr,"10.0.0.42");
    strcpy(s_service.wifi.ap_ssid,WIFI_MANAGER_AP_SSID);
    strcpy(s_service.wifi.ap_ip,WIFI_MANAGER_AP_IP);
    s_service.status.temperature = (temperature_sample_t) {
        .raw_c = 42.0f,
        .filtered_c = 42.0f,
        .valid = true,
        .consecutive_valid = 5U,
        .fault = HEAT_FAULT_NONE,
    };
}

static void start_test_server(void)
{
    reset_service();
    const web_server_config_t config = {
        .ops = {
            .get_status = fake_get_status,
            .set_target = fake_set_target,
            .set_enabled = fake_set_enabled,
            .set_manual_fan = fake_set_manual_fan,
            .ack_fault = fake_ack_fault,
            .get_firmware_status = fake_firmware_status,
            .request_firmware_update = fake_firmware_update,
            .get_wifi_status = fake_wifi_status,
            .request_wifi_change = fake_wifi_change,
        },
        .port = TEST_PORT,
    };
    TEST_ESP_OK(web_server_start(&config));
}

static int connect_local(void)
{
    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sock);

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_PORT),
    };
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    TEST_ASSERT_EQUAL_INT(0, connect(sock, (struct sockaddr *) &address,
                                     sizeof(address)));
    return sock;
}

/* Sends one request and reads until the socket goes quiet. */
static int http_exchange(const char *request, char *response, size_t capacity)
{
    const int sock = connect_local();
    const size_t length = strlen(request);
    TEST_ASSERT_EQUAL_INT((int) length, send(sock, request, length, 0));

    size_t total = 0U;
    while (total + 1U < capacity) {
        const int received = recv(sock, &response[total], capacity - total - 1U, 0);
        if (received <= 0) {
            break;
        }
        total += (size_t) received;
        response[total] = '\0';
        /* Stop once the body is in hand rather than waiting for close. */
        if (strstr(response, "\r\n\r\n") != NULL) {
            const char *body = strstr(response, "\r\n\r\n") + 4;
            const char *length_header = strstr(response, "\r\nContent-Length:");
            if (length_header != NULL) {
                const size_t body_length = strtoul(
                    length_header + strlen("\r\nContent-Length:"), NULL, 10);
                if (total >= (size_t) (body - response) + body_length) {
                    break;
                }
            } else if (strstr(response, "204 No Content") != NULL || body[0] != '\0') {
                break;
            }
        }
    }
    close(sock);
    response[total] = '\0';
    return (int) total;
}

static int status_code(const char *response)
{
    if (strncmp(response, "HTTP/1.1 ", 9) != 0) {
        return -1;
    }
    return atoi(&response[9]);
}

static char *request_with_body(const char *method,
                               const char *path,
                               const char *body)
{
    static char request[API_MAX_BODY_BYTES + 256U];
    snprintf(request, sizeof(request),
             "%s %s HTTP/1.1\r\n"
             "Host: 127.0.0.1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             method, path, (unsigned) strlen(body), body);
    return request;
}

TEST_CASE("status responds with the serialized contract", "[api]")
{
    start_test_server();

    char response[RESPONSE_CAPACITY];
    http_exchange("GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Connection: close\r\n\r\n",
                  response, sizeof(response));

    TEST_ASSERT_EQUAL_INT(200, status_code(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "application/json"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"state\":\"idle\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"target_c\":60"));

    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("diagnostics serves a complete ring without exhausting the HTTP stack", "[api]")
{
    start_test_server();
    diagnostics_reset();
    for (size_t i = 0U; i < DIAGNOSTICS_CAPACITY; ++i) {
        diagnostics_record(UINT64_MAX - DIAGNOSTICS_CAPACITY + i,
                           HEAT_STATE_HEATING, HEAT_STATE_HOLDING,
                           HEAT_FAULT_OVERTEMP);
    }

    const size_t capacity = 8192U;
    char *response = malloc(capacity);
    TEST_ASSERT_NOT_NULL(response);
    http_exchange("GET /api/diagnostics HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Connection: close\r\n\r\n", response, capacity);

    TEST_ASSERT_EQUAL_INT(200, status_code(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "application/json"));
    const char *body = strstr(response, "\r\n\r\n");
    TEST_ASSERT_NOT_NULL(body);
    body += 4;
    TEST_ASSERT_EQUAL_INT('}', body[strlen(body) - 1U]);
    size_t count = 0U;
    const char *record = body;
    while ((record = strstr(record, "\"fault\":\"overtemperature\"")) != NULL) {
        ++count;
        ++record;
    }
    TEST_ASSERT_EQUAL_UINT32(DIAGNOSTICS_CAPACITY, count);
    free(response);
    diagnostics_reset();
    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("a valid target update reaches the runtime once", "[api]")
{
    start_test_server();

    char response[RESPONSE_CAPACITY];
    http_exchange(request_with_body("PUT", "/api/target", "{\"target_c\":60.0}"),
                  response, sizeof(response));

    TEST_ASSERT_EQUAL_INT(204, status_code(response));
    TEST_ASSERT_EQUAL_INT(1, s_service.set_target_calls);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, s_service.last_target);

    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("invalid target bodies never reach the runtime", "[api]")
{
    start_test_server();

    char response[RESPONSE_CAPACITY];
    http_exchange(request_with_body("PUT", "/api/target", "{\"target_c\":60.2}"),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(400, status_code(response));

    http_exchange(request_with_body("PUT", "/api/target", "{\"target_c\":\"hot\"}"),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(400, status_code(response));

    http_exchange(request_with_body("PUT", "/api/target", "not json"),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(400, status_code(response));

    TEST_ASSERT_EQUAL_INT(0, s_service.set_target_calls);

    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("enable and acknowledge conflicts return 409", "[api]")
{
    start_test_server();
    char response[RESPONSE_CAPACITY];

    /* No fault is latched, so acknowledgement conflicts. */
    http_exchange(request_with_body("POST", "/api/fault/acknowledge", ""),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(409, status_code(response));
    TEST_ASSERT_EQUAL_INT(0, s_service.ack_calls);

    /* Too few valid samples to enable. */
    s_service.status.temperature.consecutive_valid = 1U;
    http_exchange(request_with_body("POST", "/api/control/enable", ""),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(409, status_code(response));

    /* A latched fault blocks enabling. */
    s_service.status.temperature.consecutive_valid = 5U;
    s_service.status.state = HEAT_STATE_FAULT;
    s_service.status.fault = HEAT_FAULT_SENSOR_OPEN;
    http_exchange(request_with_body("POST", "/api/control/enable", ""),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(409, status_code(response));
    TEST_ASSERT_EQUAL_INT(0, s_service.set_enabled_calls);

    /* The same fault makes acknowledgement legal. */
    http_exchange(request_with_body("POST", "/api/fault/acknowledge", ""),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(204, status_code(response));
    TEST_ASSERT_EQUAL_INT(1, s_service.ack_calls);

    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("enable and disable pass through when preconditions hold", "[api]")
{
    start_test_server();
    char response[RESPONSE_CAPACITY];

    http_exchange(request_with_body("POST", "/api/control/enable", ""),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(204, status_code(response));
    TEST_ASSERT_EQUAL_INT(1, s_service.set_enabled_calls);
    TEST_ASSERT_TRUE(s_service.last_enabled);

    http_exchange(request_with_body("POST", "/api/control/disable", ""),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(204, status_code(response));
    TEST_ASSERT_EQUAL_INT(2, s_service.set_enabled_calls);
    TEST_ASSERT_FALSE(s_service.last_enabled);

    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("an unavailable runtime queue returns 503", "[api]")
{
    start_test_server();
    s_service.command_result = ESP_ERR_INVALID_STATE;

    char response[RESPONSE_CAPACITY];
    http_exchange(request_with_body("POST", "/api/control/disable", ""),
                  response, sizeof(response));

    TEST_ASSERT_EQUAL_INT(503, status_code(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"error\":\"unavailable\""));

    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("manual fan enable and disable reach the runtime", "[api]")
{
    start_test_server();

    char response[RESPONSE_CAPACITY];
    http_exchange(request_with_body("POST", "/api/control/fan/enable", ""),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(204, status_code(response));
    TEST_ASSERT_EQUAL_INT(1, s_service.set_manual_fan_calls);
    TEST_ASSERT_TRUE(s_service.last_manual_fan);

    http_exchange(request_with_body("POST", "/api/control/fan/disable", ""),
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(204, status_code(response));
    TEST_ASSERT_EQUAL_INT(2, s_service.set_manual_fan_calls);
    TEST_ASSERT_FALSE(s_service.last_manual_fan);

    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("an sse client receives a complete status event", "[api]")
{
    start_test_server();

    const int sock = connect_local();
    static const char k_request[] =
        "GET /api/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    TEST_ASSERT_EQUAL_INT((int) (sizeof(k_request) - 1U),
                          send(sock, k_request, sizeof(k_request) - 1U, 0));

    char buffer[RESPONSE_CAPACITY] = {0};
    size_t total = 0U;
    const int64_t deadline = esp_timer_get_time() + 8000000;
    while (esp_timer_get_time() < deadline && total + 1U < sizeof(buffer)) {
        const int received =
            recv(sock, &buffer[total], sizeof(buffer) - total - 1U, 0);
        if (received > 0) {
            total += (size_t) received;
            buffer[total] = '\0';
            if (strstr(buffer, "\n\n") != NULL &&
                strstr(buffer, "event: status") != NULL) {
                break;
            }
        }
    }
    close(sock);

    TEST_ASSERT_NOT_NULL(strstr(buffer, "text/event-stream"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, "Cache-Control: no-cache"));
    const char *event = strstr(buffer, "event: status\ndata: ");
    TEST_ASSERT_NOT_NULL(event);
    TEST_ASSERT_NOT_NULL(strstr(event, "\"state\":\"idle\""));
    TEST_ASSERT_NOT_NULL(strstr(event, "\n\n"));

    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("event streams are capped at four clients", "[api]")
{
    start_test_server();

    int sockets[SSE_HUB_MAX_CLIENTS];
    static const char k_request[] =
        "GET /api/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    for (size_t i = 0U; i < SSE_HUB_MAX_CLIENTS; ++i) {
        sockets[i] = connect_local();
        send(sockets[i], k_request, sizeof(k_request) - 1U, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    TEST_ASSERT_EQUAL_UINT32(SSE_HUB_MAX_CLIENTS, sse_hub_client_count());

    char response[RESPONSE_CAPACITY];
    http_exchange("GET /api/wifi HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nConnection: close\r\n\r\n",response,sizeof(response));
    TEST_ASSERT_EQUAL_INT(200,status_code(response));
    http_exchange("GET /api/firmware HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nConnection: close\r\n\r\n",response,sizeof(response));
    TEST_ASSERT_EQUAL_INT(200,status_code(response));
    http_exchange(k_request, response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(503, status_code(response));

    for (size_t i = 0U; i < SSE_HUB_MAX_CLIENTS; ++i) {
        close(sockets[i]);
    }
    TEST_ESP_OK(web_server_stop());
}

/* Provisioning uses GET/PUT /api/wifi; legacy scan/reset endpoints remain absent. */
TEST_CASE("legacy wifi provisioning endpoints are not served", "[api]")
{
    start_test_server();

    char response[RESPONSE_CAPACITY];

    http_exchange("GET /api/wifi/scan HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Connection: close\r\n\r\n",
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(404, status_code(response));

    http_exchange("POST /api/wifi/configure HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Content-Length: 0\r\nConnection: close\r\n\r\n",
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(404, status_code(response));

    http_exchange("POST /api/wifi/reset HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Content-Length: 0\r\nConnection: close\r\n\r\n",
                  response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(404, status_code(response));

    TEST_ESP_OK(web_server_stop());
}

/* Without the captive-portal wildcard an unknown path is a plain 404, not a
 * redirect to the dashboard. */
TEST_CASE("unknown paths are not redirected to the dashboard", "[api]")
{
    start_test_server();

    char response[RESPONSE_CAPACITY];
    http_exchange("GET /generate_204 HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Connection: close\r\n\r\n",
                  response, sizeof(response));

    TEST_ASSERT_EQUAL_INT(404, status_code(response));
    TEST_ASSERT_NULL(strstr(response, "Location:"));

    TEST_ESP_OK(web_server_stop());
}

/* EMBED_TXTFILES appends a NUL terminator to every asset, so `end - start`
 * overstates the payload by one byte. HTML and CSS parsers ignore a trailing
 * NUL, but JavaScript rejects it with a SyntaxError and the dashboard never
 * runs, so the served length has to exclude it. */
TEST_CASE("embedded asset length excludes the NUL terminator", "[api]")
{
    /* The compiler terminates this exactly as EMBED_TXTFILES does, so
     * `sizeof` stands in for the linker's _end symbol. */
    static const char asset[] = "console.log(1);\n";
    const char *start = asset;
    const char *end = asset + sizeof(asset);

    TEST_ASSERT_EQUAL_UINT32(sizeof(asset) - 1U,
                             web_server_asset_length(start, end));
    TEST_ASSERT_NOT_EQUAL(0, start[web_server_asset_length(start, end) - 1U]);

    /* Degenerate spans must not underflow into a huge length. */
    TEST_ASSERT_EQUAL_UINT32(0, web_server_asset_length(start, start));
    TEST_ASSERT_EQUAL_UINT32(0, web_server_asset_length(NULL, NULL));
}

TEST_CASE("firmware status does not request a release check", "[api]")
{
    start_test_server();
    char response[RESPONSE_CAPACITY];
    http_exchange("GET /api/firmware HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nConnection: close\r\n\r\n", response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(200, status_code(response));
    TEST_ASSERT_EQUAL_INT(0, s_service.update_calls);
    TEST_ASSERT_NOT_NULL(strstr(response, "no-store"));
    cJSON *root = cJSON_Parse(strstr(response, "\r\n\r\n") + 4);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_STRING("1.0.0", cJSON_GetObjectItem(root, "installed")->valuestring);
    TEST_ASSERT_EQUAL_INT(32, strlen(cJSON_GetObjectItem(root, "csrf_token")->valuestring));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "password"));
    cJSON_Delete(root);
    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("firmware update requires same-origin token and accepts once", "[api]")
{
    start_test_server();
    /* Unity runs on the main task; fixture buffers must not consume its
     * remaining stack alongside snprintf and the loopback client. */
    static char response[RESPONSE_CAPACITY], request[512], token[33];
    http_exchange("GET /api/firmware HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nConnection: close\r\n\r\n", response, sizeof(response));
    cJSON *root = cJSON_Parse(strstr(response, "\r\n\r\n") + 4);
    TEST_ASSERT_NOT_NULL(root);
    strcpy(token, cJSON_GetObjectItem(root, "csrf_token")->valuestring);
    cJSON_Delete(root);
    snprintf(request, sizeof(request), "POST /api/firmware/update HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nOrigin: http://evil.example\r\nX-P1S-CSRF: %s\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}", token);
    http_exchange(request, response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(403, status_code(response));
    TEST_ASSERT_EQUAL_INT(0, s_service.update_calls);
    snprintf(request, sizeof(request), "POST /api/firmware/update HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nOrigin: http://127.0.0.1:8180\r\nX-P1S-CSRF: %s\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}", token);
    http_exchange(request, response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(202, status_code(response));
    TEST_ASSERT_EQUAL_INT(1, s_service.update_calls);
    s_service.command_result = ESP_ERR_NOT_ALLOWED;
    http_exchange(request, response, sizeof(response));
    TEST_ASSERT_EQUAL_INT(409, status_code(response));
    TEST_ESP_OK(web_server_stop());
}

TEST_CASE("Wi-Fi API returns visible credentials and validates trusted saves", "[api]")
{
    start_test_server();
    static char response[RESPONSE_CAPACITY], request[1300], token[33];
    http_exchange("GET /api/wifi HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nConnection: close\r\n\r\n",response,sizeof(response));
    TEST_ASSERT_EQUAL_INT(200,status_code(response));
    TEST_ASSERT_NOT_NULL(strstr(response,"Cache-Control: no-store"));
    cJSON *root=cJSON_Parse(strstr(response,"\r\n\r\n")+4);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_STRING("visible password",cJSON_GetObjectItem(root,"password")->valuestring);
    TEST_ASSERT_EQUAL_STRING(WIFI_MANAGER_AP_IP,cJSON_GetObjectItem(root,"ap_ip")->valuestring);
    strcpy(token,cJSON_GetObjectItem(root,"csrf_token")->valuestring); cJSON_Delete(root);
    static const char body[]="{\"ssid\":\" next network \",\"password\":\" next password \"}";
    snprintf(request,sizeof(request),"PUT /api/wifi HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nOrigin: http://evil.example\r\nX-P1S-CSRF: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",token,(unsigned)strlen(body),body);
    http_exchange(request,response,sizeof(response));
    TEST_ASSERT_EQUAL_INT(403,status_code(response)); TEST_ASSERT_EQUAL_INT(0,s_service.wifi_calls);
    snprintf(request,sizeof(request),"PUT /api/wifi HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nOrigin: http://127.0.0.1:8180\r\nX-P1S-CSRF: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",token,(unsigned)strlen(body),body);
    http_exchange(request,response,sizeof(response));
    TEST_ASSERT_EQUAL_INT(202,status_code(response));
    TEST_ASSERT_EQUAL_STRING(" next network ",s_service.requested_wifi.ssid);
    TEST_ASSERT_EQUAL_STRING(" next password ",s_service.requested_wifi.password);
    s_service.command_result=ESP_ERR_NOT_ALLOWED;
    http_exchange(request,response,sizeof(response)); TEST_ASSERT_EQUAL_INT(409,status_code(response));
    s_service.command_result=ESP_ERR_INVALID_STATE;
    http_exchange(request,response,sizeof(response)); TEST_ASSERT_EQUAL_INT(503,status_code(response));
    s_service.command_result=ESP_OK;
    const int before=s_service.wifi_calls;
    snprintf(request,sizeof(request),"PUT /api/wifi HTTP/1.1\r\nHost: 127.0.0.1:8180\r\nOrigin: http://127.0.0.1:8180\r\nX-P1S-CSRF: %s\r\nContent-Type: application/json\r\nContent-Length: 800\r\nConnection: close\r\n\r\n",token);
    const size_t header=strlen(request); memset(request+header,' ',800); request[header+800]=0;
    http_exchange(request,response,sizeof(response)); TEST_ASSERT_EQUAL_INT(400,status_code(response));
    TEST_ASSERT_EQUAL_INT(before,s_service.wifi_calls);
    TEST_ESP_OK(web_server_stop());
}
