#include "http_csrf.h"

#include <string.h>

#include "esp_random.h"
#include "wifi_manager.h"

static char s_token[33];

void http_csrf_init(void)
{
    if (s_token[0]) return;
    static const char hex[] = "0123456789abcdef";
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) {
        s_token[2 * i] = hex[random[i] >> 4];
        s_token[2 * i + 1] = hex[random[i] & 15];
    }
}

const char *http_csrf_token(void)
{
    return s_token;
}

static bool read_header(httpd_req_t *req, const char *name, char *out, size_t capacity)
{
    const size_t length = httpd_req_get_hdr_value_len(req, name);
    return length > 0 && length < capacity &&
        httpd_req_get_hdr_value_str(req, name, out, capacity) == ESP_OK;
}

bool http_csrf_request_trusted(httpd_req_t *req)
{
    char host[sizeof(WIFI_MANAGER_AP_IP ":80")];
    char origin[sizeof("http://" WIFI_MANAGER_AP_IP ":80")];
    char token[sizeof(s_token)];
    if (!s_token[0] || !read_header(req, "Host", host, sizeof(host)) ||
        !read_header(req, "Origin", origin, sizeof(origin)) ||
        !read_header(req, "X-P1S-CSRF", token, sizeof(token)) ||
        strlen(token) != sizeof(s_token) - 1) return false;

    unsigned difference = 0;
    for (size_t i = 0; i < sizeof(s_token) - 1; ++i) {
        difference |= (unsigned char) token[i] ^ (unsigned char) s_token[i];
    }
    return difference == 0 &&
        ((strcmp(host, WIFI_MANAGER_AP_IP) == 0 && strcmp(origin, "http://" WIFI_MANAGER_AP_IP) == 0) ||
         (strcmp(host, WIFI_MANAGER_AP_IP ":80") == 0 && strcmp(origin, "http://" WIFI_MANAGER_AP_IP ":80") == 0));
}
