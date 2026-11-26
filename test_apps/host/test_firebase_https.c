#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_transport.h"
#include "firebase_https.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

typedef enum { FAST, SLOW_DNS, SLOW_HANDSHAKE, SLOW_WRITE, SLOW_HEADERS, SLOW_BODY } timing_t;
typedef enum {
    ALLOC_OK,
    ALLOC_TRANSPORT_FAIL,
    ALLOC_HTTP_CLIENT_FAIL,
    ALLOC_DNS_SEMAPHORE_FAIL,
    ALLOC_TLS_FAIL,
} allocation_t;

static struct fake_transport {
    void *context;
    connect_func connect;
    io_read_func read;
    io_func write;
    trans_func close, destroy;
} transport;

static struct http_fixture {
    const char *url;
    char host[256];
    esp_http_client_method_t method;
    const char *body;
    size_t length;
    size_t offset;
    int64_t now_us;
    int budget_ms;
    int64_t header_step_us;
    timing_t timing;
    allocation_t allocation;
    bool in_headers;
    bool complete;
    bool cleaned_up;
    bool transport_destroyed;
} fixture;

static struct esp_tls { bool open; } tls;
static struct host_semaphore { bool ready; } dns_done;
static bool semaphore_live;
static dns_found_callback late_dns;
static void *late_dns_arg;

int64_t esp_timer_get_time(void) { return fixture.now_us; }
esp_err_t esp_crt_bundle_attach(void *config) { (void) config; return ESP_OK; }
void vTaskDelay(TickType_t ticks) { fixture.now_us += ticks * 4000LL; }

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    assert(!semaphore_live);
    if (fixture.allocation == ALLOC_DNS_SEMAPHORE_FAIL) return NULL;
    semaphore_live = true;
    dns_done.ready = false;
    return &dns_done;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &dns_done && semaphore_live);
    semaphore_live = false;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &dns_done && semaphore_live);
    semaphore->ready = true;
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    assert(semaphore == &dns_done && semaphore_live);
    if (semaphore->ready) return pdTRUE;
    vTaskDelay(ticks);
    return pdFALSE;
}

err_t tcpip_try_callback(void (*callback)(void *), void *arg)
{
    callback(arg);
    return ERR_OK;
}

err_t dns_gethostbyname_addrtype(const char *host, ip_addr_t *address, dns_found_callback callback,
                                void *arg, uint8_t family)
{
    assert(strcmp(host, fixture.host) == 0 && family == LWIP_DNS_ADDRTYPE_DEFAULT);
    if (fixture.timing == SLOW_DNS) {
        late_dns = callback;
        late_dns_arg = arg;
        return ERR_INPROGRESS;
    }
    address->address = 1;
    return ERR_OK;
}

char *ipaddr_ntoa_r(const ip_addr_t *address, char *result, int capacity)
{
    assert(address->address == 1 && capacity >= (int) sizeof("192.0.2.1"));
    return strcpy(result, "192.0.2.1");
}

esp_tls_t *esp_tls_init(void)
{
    assert(!tls.open);
    if (fixture.allocation == ALLOC_TLS_FAIL) return NULL;
    tls.open = true;
    return &tls;
}

int esp_tls_conn_new_async(const char *address, int length, int port, const esp_tls_cfg_t *config,
                           esp_tls_t *connection)
{
    assert(connection == &tls && tls.open);
    assert(strcmp(address, "192.0.2.1") == 0 && length == (int) strlen(address) && port == 443);
    assert(config->non_block && config->crt_bundle_attach == esp_crt_bundle_attach);
    assert(strcmp(config->common_name, fixture.host) == 0);
    assert(config->timeout_ms == (fixture.budget_ms * 1000LL - fixture.now_us) / 1000);
    assert((config->keep_alive_cfg != NULL) == (fixture.method == HTTP_METHOD_GET));
    if (fixture.timing != SLOW_HANDSHAKE) return 1;
    fixture.now_us += 3996000;
    return 0;
}

ssize_t esp_tls_conn_write(esp_tls_t *connection, const void *data, size_t length)
{
    assert(connection == &tls && tls.open && length == 2 && memcmp(data, "{}", length) == 0);
    return fixture.timing == SLOW_WRITE ? ESP_TLS_ERR_SSL_WANT_WRITE : (ssize_t) length;
}

ssize_t esp_tls_conn_read(esp_tls_t *connection, void *data, size_t capacity)
{
    assert(connection == &tls && tls.open && capacity);
    if (fixture.in_headers) {
        fixture.now_us += fixture.header_step_us;
        *(char *) data = ' ';
        return 1;
    }
    size_t available = fixture.length - fixture.offset;
    if (fixture.timing == SLOW_BODY && available) {
        fixture.now_us += 4000000;
        available = 1;
    }
    if (available > capacity) available = capacity;
    memcpy(data, fixture.body + fixture.offset, available);
    fixture.offset += available;
    return (ssize_t) available;
}

int esp_tls_conn_destroy(esp_tls_t *connection)
{
    assert(connection == &tls && tls.open);
    tls.open = false;
    return 0;
}

esp_transport_handle_t esp_transport_init(void)
{
    return fixture.allocation == ALLOC_TRANSPORT_FAIL ? NULL : &transport;
}

esp_err_t esp_transport_destroy(esp_transport_handle_t instance)
{
    assert(instance == &transport && !fixture.transport_destroyed);
    instance->destroy(instance);
    fixture.transport_destroyed = true;
    return ESP_OK;
}

void *esp_transport_get_context_data(esp_transport_handle_t instance) { return instance->context; }
esp_err_t esp_transport_set_context_data(esp_transport_handle_t instance, void *context)
{
    instance->context = context;
    return ESP_OK;
}

esp_err_t esp_transport_set_default_port(esp_transport_handle_t instance, int port)
{
    assert(instance == &transport && port == 443);
    return ESP_OK;
}

esp_err_t esp_transport_set_func(esp_transport_handle_t instance, connect_func connect,
                                 io_read_func read, io_func write, trans_func close,
                                 poll_func poll_read, poll_func poll_write, trans_func destroy)
{
    (void) poll_read;
    (void) poll_write;
    instance->connect = connect;
    instance->read = read;
    instance->write = write;
    instance->close = close;
    instance->destroy = destroy;
    return ESP_OK;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(strcmp(config->url, fixture.url) == 0);
    assert(config->method == fixture.method);
    assert((size_t) config->buffer_size_tx >= strlen(config->url) + sizeof("PUT  HTTP/1.1\r\n"));
    assert(config->transport == &transport && config->disable_auto_redirect);
    assert(config->timeout_ms == fixture.budget_ms);
    return fixture.allocation == ALLOC_HTTP_CLIENT_FAIL ? NULL : &fixture;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value)
{
    assert(client == &fixture);
    assert(strcmp(name, "Content-Type") == 0 && strcmp(value, "application/json") == 0);
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int length)
{
    assert(client == &fixture && length == (fixture.method == HTTP_METHOD_GET ? 0 : 2));
    return transport.connect(&transport, fixture.host, 443, fixture.budget_ms) == 0 ? ESP_OK : ESP_FAIL;
}

int esp_http_client_write(esp_http_client_handle_t client, const char *body, int length)
{
    assert(client == &fixture && length == 2 && strcmp(body, "{}") == 0);
    return transport.write(&transport, body, length, 12000);
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    if (client->timing == SLOW_HEADERS) {
        char byte;
        client->in_headers = true;
        /* The SDK loops inside fetch_headers while each read makes progress. */
        while (transport.read(&transport, &byte, 1, fixture.budget_ms) == 1) {
            assert(client->now_us <= fixture.budget_ms * 1000LL);
        }
        return -1;
    }
    if (client->timing == SLOW_BODY) client->now_us += 4000000;
    return (int64_t) client->length;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) { (void) client; return 200; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) { return client->complete; }

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int capacity)
{
    int used = 0;
    while (used < capacity && client->offset < client->length) {
        const int n = transport.read(&transport, buffer + used, capacity - used, 12000);
        if (n <= 0) break;
        used += n;
    }
    return used;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
    assert(client == &fixture);
    transport.close(&transport);
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    client->cleaned_up = true;
    return ESP_OK;
}

static void test_request(const char *url, esp_http_client_method_t method, const char *body,
                         size_t length, size_t capacity, bool complete, timing_t timing, esp_err_t expected)
{
    fixture = (struct http_fixture) {
        .url = url, .method = method, .body = body, .length = length, .complete = complete, .timing = timing,
        .budget_ms = 12000, .header_step_us = 4000000,
    };
    const char *host = url + strlen("https://");
    const size_t host_length = strcspn(host, "/");
    assert(host_length < sizeof(fixture.host));
    memcpy(fixture.host, host, host_length);
    assert(!semaphore_live && !tls.open);
    int status;
    char response[64];
    const esp_err_t result = firebase_https_request(method, url, "application/json", "{}",
        response, capacity, &status);
    assert(result == expected && fixture.cleaned_up && fixture.transport_destroyed && !tls.open);
    if (timing == FAST) assert(status == 200);
    if (expected == ESP_OK) assert(strcmp(response, body) == 0);
    if (expected == ESP_ERR_TIMEOUT) assert(fixture.now_us == 12000000);
    if (timing == SLOW_BODY) assert(fixture.offset == 2);
    if (timing == SLOW_DNS) {
        /* lwIP can finish after the request has returned. */
        assert(semaphore_live && late_dns);
        const ip_addr_t address = {.address = 1};
        late_dns(fixture.host, &address, late_dns_arg);
        late_dns = NULL;
    }
    assert(!semaphore_live);
}

static void stream_headers_share_budget_with_the_redirect(void)
{
    const char *url = "https://demo.firebaseio.com/devices/example/commands.json";
    fixture = (struct http_fixture) {
        .url = url, .host = "demo.firebaseio.com", .method = HTTP_METHOD_GET,
        .budget_ms = 6000, .header_step_us = 1000000,
    };
    firebase_https_connection_t stream;
    assert(firebase_https_init(&stream, HTTP_METHOD_GET, url, fixture.budget_ms) == ESP_OK);
    assert(esp_http_client_open(stream.http, 0) == ESP_OK);
    fixture.now_us += 2000000; /* Time spent on the first leg of the redirect. */
    assert(esp_http_client_fetch_headers(stream.http) == 0);
    assert(esp_http_client_close(stream.http) == ESP_OK);

    assert(esp_http_client_open(stream.http, 0) == ESP_OK);
    fixture.timing = SLOW_HEADERS;
    assert(esp_http_client_fetch_headers(stream.http) < 0);
    assert(fixture.now_us == 6000000);
    firebase_https_cleanup(&stream);
    assert(fixture.cleaned_up && fixture.transport_destroyed && !tls.open && !semaphore_live);
}

static void requests_recover_after_allocation_failures(void)
{
    const char *url = "https://api.example.invalid";
    const char body[] = "{\"expiresIn\":\"3600\"}";
    const struct {
        allocation_t allocation;
        bool destroy_transport;
        bool clean_up_client;
    } cases[] = {
        {ALLOC_TRANSPORT_FAIL, false, false},
        {ALLOC_HTTP_CLIENT_FAIL, true, false},
        {ALLOC_DNS_SEMAPHORE_FAIL, true, true},
        {ALLOC_TLS_FAIL, true, true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fixture = (struct http_fixture) {
            .url = url, .host = "api.example.invalid", .method = HTTP_METHOD_POST,
            .budget_ms = 12000, .allocation = cases[i].allocation,
        };
        char response[64] = "previous response";
        int status = 200;
        assert(firebase_https_request(HTTP_METHOD_POST, url, "application/json", "{}",
            response, sizeof(response), &status) == ESP_ERR_NO_MEM);
        assert(status == 0 && response[0] == '\0');
        assert(fixture.transport_destroyed == cases[i].destroy_transport);
        assert(fixture.cleaned_up == cases[i].clean_up_client);
        assert(!semaphore_live && !tls.open);

        test_request(url, HTTP_METHOD_POST, body, sizeof(body) - 1,
            sizeof(body), true, FAST, ESP_OK);
    }
}

int main(void)
{
    const char body[] = "{\"expiresIn\":\"3600\"}";
    const char *api = "https://api.example.invalid";
    test_request(api, HTTP_METHOD_POST, body, sizeof(body) - 1, sizeof(body), true, FAST, ESP_OK);
    test_request(api, HTTP_METHOD_POST, body, sizeof(body) - 1, 10, true, FAST, ESP_ERR_INVALID_SIZE);
    test_request(api, HTTP_METHOD_POST, body, sizeof(body) - 1, sizeof(body), false, FAST, ESP_ERR_INVALID_RESPONSE);
    test_request(api, HTTP_METHOD_POST, "{\0}", 3, 10, true, FAST, ESP_ERR_INVALID_RESPONSE);
    const timing_t stalls[] = {SLOW_DNS, SLOW_HANDSHAKE, SLOW_WRITE, SLOW_HEADERS, SLOW_BODY};
    for (size_t i = 0; i < sizeof(stalls) / sizeof(stalls[0]); ++i) {
        test_request(api, HTTP_METHOD_POST, body, sizeof(body) - 1, sizeof(body), true, stalls[i], ESP_ERR_TIMEOUT);
    }

    char token[FIREBASE_ID_TOKEN_BYTES];
    memset(token, 'A', sizeof(token) - 1);
    token[sizeof(token) - 1] = '\0';
    char url[sizeof(token) + 200];
    const int size = snprintf(url, sizeof(url),
        "https://demo.firebaseio.com/devices/example/results/command.json?auth=%s&print=silent", token);
    assert(size > 512 && (size_t) size < sizeof(url));
    test_request(url, HTTP_METHOD_PUT, body, sizeof(body) - 1, sizeof(body), true, FAST, ESP_OK);
    stream_headers_share_budget_with_the_redirect();
    requests_recover_after_allocation_failures();
    puts("Firebase HTTPS response handling passed");
    return 0;
}
