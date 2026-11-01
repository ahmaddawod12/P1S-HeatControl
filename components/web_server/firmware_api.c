#include "firmware_api.h"
#include "firmware_api_policy.h"
#include "firmware_policy.h"
#include "cJSON.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <stdio.h>
#include <string.h>

static web_server_ops_t s_ops;
static char s_csrf[33], s_boot_id[17];
static esp_err_t json_response(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status); httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}
static esp_err_t get_firmware(httpd_req_t *req)
{
    if (!s_ops.get_firmware_status) return json_response(req, "503 Service Unavailable", "{\"error\":\"unavailable\"}");
    firmware_status_t status; s_ops.get_firmware_status(&status);
    cJSON *root = cJSON_CreateObject();
    if (!root) return json_response(req, "503 Service Unavailable", "{\"error\":\"memory\"}");
    cJSON_AddStringToObject(root, "installed", status.installed);
    cJSON_AddStringToObject(root, "candidate", status.candidate);
    cJSON_AddStringToObject(root, "phase", firmware_phase_name(status.phase));
    cJSON_AddStringToObject(root, "result", firmware_result_name(status.result));
    cJSON_AddNumberToObject(root, "received", status.received);
    cJSON_AddNumberToObject(root, "total", status.total);
    cJSON_AddBoolToObject(root, "maintenance", status.maintenance);
    cJSON_AddBoolToObject(root, "trial_pending", status.trial_pending);
    cJSON_AddStringToObject(root, "boot_id", s_boot_id);
    cJSON_AddStringToObject(root, "csrf_token", s_csrf);
    char json[1024];
    const bool printed = cJSON_PrintPreallocated(root, json, sizeof(json), false);
    cJSON_Delete(root);
    return printed ? json_response(req, "200 OK", json) :
        json_response(req, "500 Internal Server Error", "{\"error\":\"internal\"}");
}
static bool header(httpd_req_t *req, const char *name, char *out, size_t capacity)
{
    const size_t length = httpd_req_get_hdr_value_len(req, name);
    return length > 0 && length < capacity && httpd_req_get_hdr_value_str(req, name, out, capacity) == ESP_OK;
}
const char *firmware_api_csrf_token(void) { return s_csrf; }
bool firmware_api_request_trusted(httpd_req_t *req)
{
    char host[80], origin[88], csrf[33], local[16];
    if (!header(req, "Host", host, sizeof(host)) || !header(req, "Origin", origin, sizeof(origin)) ||
        !header(req, "X-P1S-CSRF", csrf, sizeof(csrf)) || strlen(csrf) != 32) return false;
    unsigned difference = 0;
    for (unsigned i = 0; i < 32; ++i) difference |= (unsigned char) csrf[i] ^ (unsigned char) s_csrf[i];
    if (difference) return false;
    struct sockaddr_storage address; socklen_t size = sizeof(address);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *) &address, &size) != 0) return false;
    struct in_addr ipv4; uint16_t port;
    if (address.ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *) &address;
        ipv4 = v4->sin_addr; port = ntohs(v4->sin_port);
#if LWIP_IPV6
    } else if (address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *) &address;
        if (!IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) return false;
        memcpy(&ipv4, &v6->sin6_addr.s6_addr[12], sizeof(ipv4));
        port = ntohs(v6->sin6_port);
#endif
    } else return false;
    if (!inet_ntop(AF_INET, &ipv4, local, sizeof(local))) return false;
    return firmware_request_origin_valid(host, origin, local, port);
}
static esp_err_t request_update(httpd_req_t *req)
{
    if (!firmware_api_request_trusted(req)) return json_response(req, "403 Forbidden", "{\"error\":\"request_origin\"}");
    char type[64];
    if (!header(req, "Content-Type", type, sizeof(type)) ||
        (strcmp(type, "application/json") != 0 && strncmp(type, "application/json;", 17) != 0) ||
        req->content_len < 2 || req->content_len > 64)
        return json_response(req, "400 Bad Request", "{\"error\":\"invalid_body\"}");
    char body[65]; size_t received = 0;
    while (received < req->content_len) {
        const int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) return json_response(req, "408 Request Timeout", "{\"error\":\"body_timeout\"}");
        received += (size_t) count;
    }
    if (!firmware_request_body_valid(body, received)) return json_response(req, "400 Bad Request", "{\"error\":\"invalid_body\"}");
    if (!s_ops.request_firmware_update) return json_response(req, "503 Service Unavailable", "{\"error\":\"unavailable\"}");
    const esp_err_t err = s_ops.request_firmware_update();
    if (err == ESP_OK) return json_response(req, "202 Accepted", "{\"accepted\":true}");
    if (err == ESP_ERR_NOT_ALLOWED) return json_response(req, "409 Conflict", "{\"error\":\"update_busy\"}");
    return json_response(req, "503 Service Unavailable", "{\"error\":\"unavailable\"}");
}
esp_err_t firmware_api_register(httpd_handle_t server, const web_server_ops_t *ops)
{
    if (!server || !ops) return ESP_ERR_INVALID_ARG;
    s_ops = *ops;
    if (!s_csrf[0]) {
        static const char hex[] = "0123456789abcdef";
        uint8_t random[24]; esp_fill_random(random, sizeof(random));
        for (unsigned i = 0; i < 16; ++i) { s_csrf[2*i] = hex[random[i] >> 4]; s_csrf[2*i+1] = hex[random[i] & 15]; }
        for (unsigned i = 0; i < 8; ++i) { s_boot_id[2*i] = hex[random[16+i] >> 4]; s_boot_id[2*i+1] = hex[random[16+i] & 15]; }
    }
    const httpd_uri_t get = {.uri = "/api/firmware", .method = HTTP_GET, .handler = get_firmware};
    const httpd_uri_t post = {.uri = "/api/firmware/update", .method = HTTP_POST, .handler = request_update};
    esp_err_t err = httpd_register_uri_handler(server, &get);
    return err == ESP_OK ? httpd_register_uri_handler(server, &post) : err;
}
