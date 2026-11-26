#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "setup_portal.h"
#include "wifi_manager.h"

const char wifi_page[] asm("_binary_wifi_setup_html_start") = "<h1>Wi-Fi setup</h1>";
const char pairing_page[] asm("_binary_pairing_setup_html_start") = "<h1>Pair controller</h1>";

static struct esp_netif { int unused; } ap;
static httpd_uri_t routes[5];
static size_t route_count;
static bool server_started, ap_available;
static wifi_setup_mode_t mode = WIFI_SETUP_NETWORK;
static bool pair_on_receive, receive_timeout;
static const char *origin = "http://" WIFI_MANAGER_AP_IP;
static const char *token;
static const char *body = "{\"ssid\":\"home\",\"password\":\"password\"}";
static char response[512];
static char response_board_id[WIFI_MANAGER_BOARD_ID_LEN];
static unsigned status_code, saves, reads, code_reads;

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{
    assert(strcmp(key, "WIFI_AP_DEF") == 0);
    return ap_available ? &ap : NULL;
}

esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *netif, char *name)
{
    assert(netif == &ap);
    strcpy(name, "ap0");
    return ESP_OK;
}

void esp_fill_random(void *out, size_t size) { memset(out, 0xAB, size); }
int64_t esp_timer_get_time(void) { return 0; }

void wifi_manager_get_status(wifi_manager_status_t *out)
{
    *out = (wifi_manager_status_t) {
        .setup_mode = mode, .ssid = "home",
        .controller_name = "saved-name", .board_id = "7C87CE01020A", .result = WIFI_RESULT_CONNECTING,
        .can_save = mode != WIFI_SETUP_NONE,
        .save_id = 42, .save_result = WIFI_SAVE_SAVED,
    };
}

esp_err_t wifi_manager_request_credentials(const wifi_credentials_t *credentials, uint32_t *id)
{
    if (mode == WIFI_SETUP_NONE) return ESP_ERR_NOT_ALLOWED;
    assert(strcmp(credentials->ssid, "home") == 0 && strcmp(credentials->password, "password") == 0);
    ++saves;
    *id = 42;
    return ESP_OK;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    assert(!server_started && config->if_name && strcmp(config->if_name->ifr_name, "ap0") == 0);
    assert(config->max_uri_handlers == sizeof(routes) / sizeof(routes[0]));
    server_started = true;
    *handle = &ap;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle)
{
    assert(server_started && handle == &ap);
    server_started = false;
    route_count = 0;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *route)
{
    assert(handle == &ap && route_count < sizeof(routes) / sizeof(routes[0]));
    routes[route_count++] = *route;
    return ESP_OK;
}

static const char *header(const char *name)
{
    if (strcmp(name, "Host") == 0) return WIFI_MANAGER_AP_IP;
    if (strcmp(name, "Origin") == 0) return origin;
    if (strcmp(name, "X-P1S-CSRF") == 0) return token;
    if (strcmp(name, "Content-Type") == 0) return "application/json";
    return NULL;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *name)
{
    (void)req;
    const char *value = header(name);
    return value ? strlen(value) : 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *out, size_t cap)
{
    const size_t length = httpd_req_get_hdr_value_len(req, name);
    if (!length || length >= cap) return ESP_FAIL;
    memcpy(out, header(name), length + 1);
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    assert(sscanf(status, "%u", &status_code) == 1);
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{ (void)req; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value)
{
    (void)req;
    if (strcmp(name, "X-P1S-Board-ID") == 0) {
        assert(strlen(value) == sizeof(response_board_id) - 1);
        strcpy(response_board_id, value);
    }
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *text)
{ (void)req; assert(strlen(text) < sizeof(response)); strcpy(response, text); return ESP_OK; }

int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 7; }
int getsockopt(int fd, int level, int option, void *value, socklen_t *size)
{
    assert(fd == 7 && level == SOL_SOCKET && option == SO_RCVTIMEO && *size == sizeof(struct timeval));
    *(struct timeval *) value = (struct timeval) {.tv_sec = 5};
    return 0;
}
int setsockopt(int fd, int level, int option, const void *value, socklen_t size)
{
    (void)value;
    assert(fd == 7 && level == SOL_SOCKET && option == SO_RCVTIMEO && size == sizeof(struct timeval));
    return 0;
}
int httpd_req_recv(httpd_req_t *req, char *out, size_t length)
{
    (void)req;
    ++reads;
    if (receive_timeout) return HTTPD_SOCK_ERR_TIMEOUT;
    if (pair_on_receive) mode = WIFI_SETUP_NONE;
    assert(length == strlen(body));
    memcpy(out, body, length);
    return (int)length;
}

static setup_pairing_state_t pairing_state;
static setup_pairing_state_t pairing_code(char code[SETUP_PAIRING_CODE_BYTES])
{
    ++code_reads;
    if (pairing_state == SETUP_PAIRING_CODE) strcpy(code, "test-code");
    return pairing_state;
}

static unsigned request(httpd_method_t method, const char *path)
{
    status_code = 0;
    response[0] = '\0';
    response_board_id[0] = '\0';
    for (size_t i = 0; i < route_count; ++i) {
        if (routes[i].method == method && strcmp(routes[i].uri, path) == 0) {
            httpd_req_t req = {.method = method, .content_len = method == HTTP_PUT ? strlen(body) : 0};
            (void)routes[i].handler(&req);
            assert(status_code);
            return status_code;
        }
    }
    return 404;
}

int main(void)
{
    assert(setup_portal_start(pairing_code) == ESP_ERR_INVALID_STATE);
    assert(!server_started);
    ap_available = true;
    assert(setup_portal_start(pairing_code) == ESP_OK && route_count == 5);

    assert(request(HTTP_GET, "/") == 200 && strcmp(response, wifi_page) == 0);
    assert(request(HTTP_GET, "/api/wifi") == 200 && strstr(response, "\"ssid\":\"home\""));
    assert(strcmp(response_board_id, "7C87CE01020A") == 0);
    assert(strstr(response, "\"pairing\":false"));
    assert(!strstr(response, "password") && !strstr(response, "saved-name"));
    assert(request(HTTP_GET, "/api/pairing") == 404 && !code_reads);
    assert(request(HTTP_PUT, "/api/wifi") == 403 && !reads && !saves);
    token = "abababababababababababababababab";
    origin = "http://example.invalid";
    assert(request(HTTP_PUT, "/api/wifi") == 403 && !reads && !saves);
    origin = "http://" WIFI_MANAGER_AP_IP;
    receive_timeout = true;
    assert(request(HTTP_PUT, "/api/wifi") == 408 && reads == 1 && !saves);
    receive_timeout = false;
    assert(request(HTTP_PUT, "/api/wifi") == 202 && saves == 1);
    assert(strcmp(response, "{\"save_id\":42}") == 0);

    mode = WIFI_SETUP_PAIRING;
    assert(request(HTTP_GET, "/") == 200 && strcmp(response, pairing_page) == 0);
    assert(request(HTTP_GET, "/wifi") == 200 && strcmp(response, wifi_page) == 0);
    assert(request(HTTP_GET, "/api/pairing") == 200 && strcmp(response, "test-code") == 0);
    assert(strcmp(response_board_id, "7C87CE01020A") == 0);
    pairing_state = SETUP_PAIRING_USB_REQUIRED;
    assert(request(HTTP_GET, "/api/pairing") == 409 && strstr(response, "USB enrollment"));
    pairing_state = SETUP_PAIRING_ACCOUNT_DISABLED;
    assert(request(HTTP_GET, "/api/pairing") == 409 && strstr(response, "administrator"));
    assert(!strstr(response, "test-code"));
    assert(request(HTTP_GET, "/api/wifi") == 200 && strstr(response, "\"pairing\":true"));
    assert(!strstr(response, "password"));
    const unsigned reads_before_recovery = reads;
    token = NULL;
    assert(request(HTTP_PUT, "/api/wifi") == 403 && reads == reads_before_recovery && saves == 1);
    token = "abababababababababababababababab";
    assert(request(HTTP_PUT, "/api/wifi") == 202 && saves == 2);

    /* Enrollment can finish while the setup server is receiving a save. */
    const unsigned code_reads_before_pairing = code_reads;
    pair_on_receive = true;
    assert(request(HTTP_PUT, "/api/wifi") == 404 && saves == 2);
    assert(request(HTTP_GET, "/") == 404);
    assert(request(HTTP_GET, "/wifi") == 404);
    assert(request(HTTP_GET, "/api/pairing") == 404 && code_reads == code_reads_before_pairing);
    assert(request(HTTP_GET, "/api/wifi") == 404);
    assert(request(HTTP_PUT, "/api/wifi") == 404);
    assert(request(HTTP_GET, "/api/status") == 404);
    assert(setup_portal_stop() == ESP_OK && !server_started);
    puts("Setup portal routes passed");
    return 0;
}
