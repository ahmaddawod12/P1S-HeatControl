#include "sse_hub.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sse";

static const char k_stream_headers[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

typedef struct {
    httpd_req_t *request;
    httpd_handle_t server;
    int sockfd;
    bool in_use;
} sse_client_t;

static sse_client_t s_clients[SSE_HUB_MAX_CLIENTS];
static SemaphoreHandle_t s_lock;

static SemaphoreHandle_t lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock;
}

/* Caller holds the lock. */
static void release_client(sse_client_t *client)
{
    if (!client->in_use) {
        return;
    }
    (void) httpd_req_async_handler_complete(client->request);
    memset(client, 0, sizeof(*client));
}

static bool send_to(sse_client_t *client, const char *frame, size_t length)
{
    size_t sent = 0U;
    while (sent < length) {
        const int written = httpd_socket_send(client->server, client->sockfd,
                                              &frame[sent], length - sent, 0);
        if (written <= 0) {
            return false;
        }
        sent += (size_t) written;
    }
    return true;
}

esp_err_t sse_hub_add_client(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t handle = lock();
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(handle, portMAX_DELAY);
    sse_client_t *slot = NULL;
    for (size_t i = 0U; i < SSE_HUB_MAX_CLIENTS; ++i) {
        if (!s_clients[i].in_use) {
            slot = &s_clients[i];
            break;
        }
    }
    if (slot == NULL) {
        xSemaphoreGive(handle);
        return ESP_ERR_NO_MEM;
    }

    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        xSemaphoreGive(handle);
        return err;
    }

    slot->request = async_req;
    slot->server = async_req->handle;
    slot->sockfd = httpd_req_to_sockfd(async_req);
    slot->in_use = true;

    /* The stream owns the response, so headers go out on the raw socket. */
    if (!send_to(slot, k_stream_headers, sizeof(k_stream_headers) - 1U)) {
        ESP_LOGW(TAG, "stream header write failed");
        release_client(slot);
        err = ESP_FAIL;
    }
    xSemaphoreGive(handle);
    return err;
}

void sse_hub_broadcast(const char *frame, size_t length)
{
    if (frame == NULL || length == 0U) {
        return;
    }
    SemaphoreHandle_t handle = lock();
    if (handle == NULL) {
        return;
    }

    xSemaphoreTake(handle, portMAX_DELAY);
    for (size_t i = 0U; i < SSE_HUB_MAX_CLIENTS; ++i) {
        if (!s_clients[i].in_use) {
            continue;
        }
        if (!send_to(&s_clients[i], frame, length)) {
            ESP_LOGI(TAG, "dropping stream on socket %d", s_clients[i].sockfd);
            release_client(&s_clients[i]);
        }
    }
    xSemaphoreGive(handle);
}

size_t sse_hub_client_count(void)
{
    SemaphoreHandle_t handle = lock();
    if (handle == NULL) {
        return 0U;
    }
    xSemaphoreTake(handle, portMAX_DELAY);
    size_t count = 0U;
    for (size_t i = 0U; i < SSE_HUB_MAX_CLIENTS; ++i) {
        if (s_clients[i].in_use) {
            ++count;
        }
    }
    xSemaphoreGive(handle);
    return count;
}

void sse_hub_close_all(void)
{
    SemaphoreHandle_t handle = lock();
    if (handle == NULL) {
        return;
    }
    xSemaphoreTake(handle, portMAX_DELAY);
    for (size_t i = 0U; i < SSE_HUB_MAX_CLIENTS; ++i) {
        release_client(&s_clients[i]);
    }
    xSemaphoreGive(handle);
}
