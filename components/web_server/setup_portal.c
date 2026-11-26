#include "setup_portal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "http_csrf.h"
#include "http_request.h"
#include "lwip/sockets.h"
#include "wifi_api_policy.h"
#include "wifi_manager.h"

extern const char wifi_setup_html_start[] asm("_binary_wifi_setup_html_start");
extern const char pairing_setup_html_start[] asm("_binary_pairing_setup_html_start");

static httpd_handle_t s_server;
static setup_pairing_code_fn s_get_code;
static struct ifreq s_ap_interface;

static esp_err_t respond(httpd_req_t *req, const char *status, const char *type, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t unavailable_route(httpd_req_t *req)
{
    return http_request_close_json(req, "404 Not Found", "{\"error\":\"setup_mode\"}");
}

static esp_err_t respond_page(httpd_req_t *req, const char *page)
{
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return respond(req, "200 OK", "text/html; charset=utf-8", page);
}

static esp_err_t get_index(httpd_req_t *req)
{
    if (!http_request_require_empty_body(req)) return ESP_FAIL;

    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);
    const char *page;
    switch (wifi.setup_mode) {
    case WIFI_SETUP_NETWORK: page = wifi_setup_html_start; break;
    case WIFI_SETUP_PAIRING: page = pairing_setup_html_start; break;
    default: return unavailable_route(req);
    }
    return respond_page(req, page);
}

static esp_err_t get_wifi_page(httpd_req_t *req)
{
    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.setup_mode == WIFI_SETUP_NONE) return unavailable_route(req);
    if (!http_request_require_empty_body(req)) return ESP_FAIL;
    return respond_page(req, wifi_setup_html_start);
}

static esp_err_t get_pairing(httpd_req_t *req)
{
    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.setup_mode != WIFI_SETUP_PAIRING) return unavailable_route(req);
    if (!http_request_require_empty_body(req)) return ESP_FAIL;

    httpd_resp_set_hdr(req, "X-P1S-Board-ID", wifi.board_id);
    char code[SETUP_PAIRING_CODE_BYTES] = {0};
    const setup_pairing_state_t state = s_get_code(code);
    if (state == SETUP_PAIRING_USB_REQUIRED) {
        return respond(req, "409 Conflict", "text/plain; charset=utf-8", "USB enrollment recovery required.");
    }
    if (state == SETUP_PAIRING_ACCOUNT_DISABLED) {
        return respond(req, "409 Conflict", "text/plain; charset=utf-8",
                       "This controller's Firebase account is disabled. Ask an administrator to re-enable it.");
    }
    return respond(req, "200 OK", "text/plain; charset=utf-8", code);
}

static const char *save_result_name(wifi_save_result_t result)
{
    switch (result) {
    case WIFI_SAVE_PENDING: return "pending";
    case WIFI_SAVE_SAVED: return "saved";
    case WIFI_SAVE_FAILED: return "save_failed";
    case WIFI_SAVE_AP_FAILED: return "ap_failed";
    default: return "none";
    }
}

static esp_err_t get_wifi(httpd_req_t *req)
{
    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.setup_mode == WIFI_SETUP_NONE) return unavailable_route(req);
    if (!http_request_require_empty_body(req)) return ESP_FAIL;

    httpd_resp_set_hdr(req, "X-P1S-Board-ID", wifi.board_id);
    char body[512];
    cJSON *root = cJSON_CreateObject();
    const bool printed = root &&
        cJSON_AddStringToObject(root, "ssid", wifi.ssid) &&
        cJSON_AddStringToObject(root, "csrf_token", http_csrf_token()) &&
        cJSON_AddBoolToObject(root, "can_save", wifi.can_save) &&
        cJSON_AddBoolToObject(root, "applying", wifi.applying) &&
        cJSON_AddBoolToObject(root, "pairing", wifi.setup_mode == WIFI_SETUP_PAIRING) &&
        cJSON_AddStringToObject(root, "result", wifi_manager_result_name(wifi.result)) &&
        cJSON_AddNumberToObject(root, "save_id", wifi.save_id) &&
        cJSON_AddStringToObject(root, "save_result", save_result_name(wifi.save_result)) &&
        cJSON_PrintPreallocated(root, body, sizeof(body), false);
    cJSON_Delete(root);
    return printed ? respond(req, "200 OK", "application/json", body)
                   : respond(req, "503 Service Unavailable", "application/json", "{\"error\":\"unavailable\"}");
}

static esp_err_t save_wifi(httpd_req_t *req)
{
    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.setup_mode == WIFI_SETUP_NONE) return unavailable_route(req);
    if (!http_csrf_request_trusted(req))
        return http_request_close_json(req, "403 Forbidden", "{\"error\":\"request_origin\"}");

    char type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        (strcmp(type, "application/json") != 0 && strncmp(type, "application/json;", 17) != 0))
        return http_request_close_json(req, "400 Bad Request", "{\"error\":\"invalid_body\"}");

    char body[WIFI_API_MAX_BODY + 1];
    wifi_credentials_t credentials;
    const esp_err_t read_err = http_request_receive_body(req, body, sizeof(body));
    if (read_err == ESP_ERR_TIMEOUT)
        return http_request_close_json(req, "408 Request Timeout", "{\"error\":\"body_timeout\"}");
    if (read_err != ESP_OK || !wifi_request_parse(body, req->content_len, &credentials))
        return http_request_close_json(req, "400 Bad Request", "{\"error\":\"invalid_body\"}");

    uint32_t save_id = 0;
    const esp_err_t err = wifi_manager_request_credentials(&credentials, &save_id);
    if (err == ESP_OK) {
        char receipt[48];
        snprintf(receipt, sizeof(receipt), "{\"save_id\":%lu}", (unsigned long) save_id);
        return respond(req, "202 Accepted", "application/json", receipt);
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        wifi_manager_get_status(&wifi);
        if (wifi.setup_mode == WIFI_SETUP_NONE) return unavailable_route(req);
        return respond(req, "409 Conflict", "application/json", "{\"error\":\"operation_busy\"}");
    }
    return respond(req, "503 Service Unavailable", "application/json", "{\"error\":\"unavailable\"}");
}

esp_err_t setup_portal_start(setup_pairing_code_fn get_code)
{
    if (!get_code) return ESP_ERR_INVALID_ARG;
    if (s_server && setup_portal_stop() != ESP_OK) return ESP_ERR_INVALID_STATE;

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_netif_get_netif_impl_name(ap, s_ap_interface.ifr_name);
    if (err != ESP_OK || !s_ap_interface.ifr_name[0]) return ESP_ERR_INVALID_STATE;

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_index},
        {.uri = "/wifi", .method = HTTP_GET, .handler = get_wifi_page},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = get_wifi},
        {.uri = "/api/wifi", .method = HTTP_PUT, .handler = save_wifi},
        {.uri = "/api/pairing", .method = HTTP_GET, .handler = get_pairing},
    };
    s_get_code = get_code;
    http_csrf_init();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.max_uri_handlers = sizeof(routes) / sizeof(routes[0]);
    config.lru_purge_enable = true;
    config.if_name = &s_ap_interface;
    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            (void) setup_portal_stop();
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t setup_portal_stop(void)
{
    if (!s_server) return ESP_OK;
    const esp_err_t err = httpd_stop(s_server);
    if (err == ESP_OK) s_server = NULL;
    return err;
}
