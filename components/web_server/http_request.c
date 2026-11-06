#include "http_request.h"

#include <string.h>

#include "esp_timer.h"
#include "lwip/sockets.h"

#define BODY_RECEIVE_DEADLINE_US 5000000LL

esp_err_t http_request_close_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    (void) httpd_resp_sendstr(req, json);
    return ESP_FAIL;
}

bool http_request_require_empty_body(httpd_req_t *req)
{
    if (req->content_len == 0U) return true;
    (void) http_request_close_json(req, "400 Bad Request", "{\"error\":\"invalid_body\"}");
    return false;
}

esp_err_t http_request_receive_body(httpd_req_t *req, char *body, size_t capacity)
{
    if (req == NULL || body == NULL || capacity == 0U) return ESP_ERR_INVALID_ARG;
    if (req->content_len >= capacity) return ESP_ERR_INVALID_SIZE;
    body[0] = '\0';
    if (req->content_len == 0U) return ESP_OK;

    const int64_t deadline = esp_timer_get_time() + BODY_RECEIVE_DEADLINE_US;
    const int fd = httpd_req_to_sockfd(req);
    struct timeval previous;
    socklen_t previous_size = sizeof(previous);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, &previous_size) != 0) return ESP_FAIL;

    esp_err_t result = ESP_OK;
    size_t received = 0U;
    while (received < req->content_len) {
        const int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) { result = ESP_ERR_TIMEOUT; break; }
        /* Round up to a millisecond: lwIP interprets a zero timeout as infinite. */
        const int64_t remaining_ms = (remaining_us + 999) / 1000;
        const struct timeval timeout = {
            .tv_sec = (long) (remaining_ms / 1000),
            .tv_usec = (long) (remaining_ms % 1000) * 1000,
        };
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
            result = ESP_FAIL; break;
        }
        const int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT || esp_timer_get_time() >= deadline) {
            result = ESP_ERR_TIMEOUT; break;
        }
        if (count <= 0) { result = ESP_FAIL; break; }
        received += (size_t) count;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, sizeof(previous)) != 0) result = ESP_FAIL;
    if (result != ESP_OK) return result;
    if (memchr(body, '\0', received) != NULL) return ESP_ERR_INVALID_ARG;
    body[received] = '\0';
    return ESP_OK;
}
