#include <winsock2.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sse_hub.h"
#include "freertos/semphr.h"

struct host_semaphore { pthread_mutex_t mutex; };
static unsigned dynamic_creations, static_creations, completions;
static int send_mode;
static bool partial_sent;
static atomic_bool header_entered, broadcast_started;
static bool concurrent;
static bool fail_init;

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    ++dynamic_creations;
    SemaphoreHandle_t lock = malloc(sizeof(*lock));
    assert(lock && pthread_mutex_init(&lock->mutex, NULL) == 0);
    return lock;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
    ++static_creations;
    if (fail_init) return NULL;
    if (!storage->handle) {
        storage->handle = malloc(sizeof(*storage->handle));
        assert(storage->handle && pthread_mutex_init(&storage->handle->mutex, NULL) == 0);
    }
    return storage->handle;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, TickType_t wait) {
    (void)wait;
    assert(lock && pthread_mutex_lock(&lock->mutex) == 0);
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t lock) {
    assert(pthread_mutex_unlock(&lock->mutex) == 0);
    return pdTRUE;
}
esp_err_t httpd_req_async_handler_begin(httpd_req_t *req, httpd_req_t **out) {
    *out = malloc(sizeof(**out));
    assert(*out);
    **out = *req;
    return ESP_OK;
}
/* IDF completes the copied request without closing its connection. */
esp_err_t httpd_req_async_handler_complete(httpd_req_t *req) {
    ++completions;
    free(req);
    return ESP_OK;
}
int httpd_req_to_sockfd(httpd_req_t *req) { return req->fd; }
int httpd_socket_send(httpd_handle_t server, int fd, const char *data, size_t size, int flags) {
    (void)server;
    if (concurrent && size >= 5 && !memcmp(data, "HTTP/", 5)) {
        atomic_store(&header_entered, true);
        while (!atomic_load(&broadcast_started)) Sleep(1);
    }
    if (send_mode == 1 || (send_mode == 2 && partial_sent)) return HTTPD_SOCK_ERR_TIMEOUT;
    if (send_mode == 2) { size = 3; partial_sent = true; }
    return send((SOCKET)fd, data, (int)size, flags);
}
typedef struct { SOCKET browser, server; httpd_req_t req; } connection_t;
static connection_t connection(void) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener != INVALID_SOCKET);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    int length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    connection_t c = {.browser = socket(AF_INET, SOCK_STREAM, 0)};
    assert(connect(c.browser, (struct sockaddr *)&address, sizeof(address)) == 0);
    c.server = accept(listener, NULL, NULL);
    assert(c.server != INVALID_SOCKET);
    closesocket(listener);
    DWORD timeout = 300;
    assert(setsockopt(c.browser, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0);
    c.req = (httpd_req_t){.handle = (void *)1, .fd = (int)c.server};
    return c;
}
static void headers(connection_t *c) {
    char data[256];
    int size = recv(c->browser, data, sizeof(data), 0);
    assert(size > 15 && !memcmp(data, "HTTP/1.1 200 OK", 15));
}
static void eof(connection_t *c) {
    char data[512];
    int size;
    do { size = recv(c->browser, data, sizeof(data), 0); } while (size > 0);
    assert(size == 0); /* A timeout leaves EventSource stuck instead of reconnecting. */
    closesocket(c->browser);
    closesocket(c->server);
}
static void *add_client(void *arg) {
    assert(sse_hub_add_client(&((connection_t *)arg)->req) == ESP_OK);
    return NULL;
}
static void *broadcast(void *arg) {
    (void)arg;
    while (!atomic_load(&header_entered)) Sleep(1);
    atomic_store(&broadcast_started, true);
    assert(sse_hub_client_count() == 1);
    static const char frame[] = "event: status\ndata: {}\n\n";
    sse_hub_broadcast(frame, sizeof(frame) - 1);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "uninitialized")) {
        assert(sse_hub_client_count() == 0);
        assert(dynamic_creations == 0 && static_creations == 0);
        httpd_req_t req = {0};
        assert(sse_hub_add_client(&req) == ESP_ERR_INVALID_STATE);
        return 0;
    }
    if (!strcmp(argv[1], "init_failure")) {
        fail_init = true;
        assert(sse_hub_init() == ESP_ERR_NO_MEM);
        assert(sse_hub_client_count() == 0);
        httpd_req_t req = {0};
        assert(sse_hub_add_client(&req) == ESP_ERR_INVALID_STATE);
        fail_init = false;
        assert(sse_hub_init() == ESP_OK);
        assert(sse_hub_init() == ESP_OK);
        assert(dynamic_creations == 0 && static_creations == 2);
        return 0;
    }
    assert(sse_hub_init() == ESP_OK);
    assert(sse_hub_init() == ESP_OK);
    WSADATA startup;
    assert(WSAStartup(MAKEWORD(2, 2), &startup) == 0);
    connection_t c = connection();
    if (!strcmp(argv[1], "concurrent")) {
        concurrent = true;
        pthread_t add, send;
        assert(pthread_create(&add, NULL, add_client, &c) == 0);
        assert(pthread_create(&send, NULL, broadcast, NULL) == 0);
        pthread_join(add, NULL);
        pthread_join(send, NULL);
        headers(&c);
        assert(dynamic_creations == 0 && static_creations == 1);
    } else if (!strcmp(argv[1], "header")) {
        send_mode = 1;
        assert(sse_hub_add_client(&c.req) == ESP_FAIL);
    } else {
        assert(sse_hub_add_client(&c.req) == ESP_OK);
        headers(&c);
        if (!strcmp(argv[1], "timeout") || !strcmp(argv[1], "partial")) {
            send_mode = !strcmp(argv[1], "partial") ? 2 : 1;
            sse_hub_broadcast("event: status\ndata: {}\n\n", 24);
        }
    }
    sse_hub_close_all();
    assert(sse_hub_client_count() == 0 && completions == 1);
    eof(&c);
    /* A replacement EventSource obtains a working stream and releases once. */
    send_mode = 0;
    concurrent = false;
    c = connection();
    assert(sse_hub_add_client(&c.req) == ESP_OK);
    headers(&c);
    sse_hub_close_all();
    eof(&c);
    assert(completions == 2);
    WSACleanup();
    puts("SSE socket lifecycle passed");
    return 0;
}
