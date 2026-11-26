#include "firebase_https.h"

#include "sdkconfig.h"

#ifdef CONFIG_P1S_FIREBASE_ENABLED

#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

#define REQUEST_BUDGET_MS 12000

typedef struct {
    SemaphoreHandle_t done;
    atomic_uint references;
    ip_addr_t address;
    bool resolved;
    char host[];
} dns_query_t;

static int remaining_ms(const firebase_https_connection_t *wire)
{
    const int64_t left_us = wire->deadline_us - esp_timer_get_time();
    return left_us > 0 ? (int) (left_us / 1000) : 0;
}

/* The DNS callback owns its reference even if the caller stops waiting. */
static void release_dns_query(dns_query_t *query)
{
    if (atomic_fetch_sub(&query->references, 1) == 1) {
        vSemaphoreDelete(query->done);
        free(query);
    }
}

static void dns_finished(const char *host, const ip_addr_t *address, void *arg)
{
    (void) host;
    dns_query_t *query = arg;
    if (address) query->address = *address;
    query->resolved = address != NULL;
    xSemaphoreGive(query->done);
    release_dns_query(query);
}

static void start_dns_query(void *arg)
{
    dns_query_t *query = arg;
    if (atomic_load(&query->references) == 1) {
        release_dns_query(query);
        return;
    }
    ip_addr_t address;
    const err_t err = dns_gethostbyname_addrtype(query->host, &address, dns_finished, query,
                                                  LWIP_DNS_ADDRTYPE_DEFAULT);
    if (err != ERR_INPROGRESS) dns_finished(query->host, err == ERR_OK ? &address : NULL, query);
}

static esp_err_t resolve_address(const firebase_https_connection_t *wire, const char *host,
                                 char address[IPADDR_STRLEN_MAX])
{
    if (!remaining_ms(wire)) return ESP_ERR_TIMEOUT;
    dns_query_t *query = calloc(1, sizeof(*query) + strlen(host) + 1);
    if (!query) return ESP_ERR_NO_MEM;
    query->done = xSemaphoreCreateBinary();
    if (!query->done) {
        free(query);
        return ESP_ERR_NO_MEM;
    }
    atomic_init(&query->references, 2);
    strcpy(query->host, host);

    esp_err_t result = ESP_ERR_NO_MEM;
    if (tcpip_try_callback(start_dns_query, query) == ERR_OK) {
        const int left = remaining_ms(wire);
        const TickType_t ticks = pdMS_TO_TICKS(left);
        if (!left || xSemaphoreTake(query->done, ticks ? ticks : 1) != pdTRUE) {
            result = ESP_ERR_TIMEOUT;
        } else {
            result = query->resolved && ipaddr_ntoa_r(&query->address, address, IPADDR_STRLEN_MAX)
                ? ESP_OK : ESP_ERR_NOT_FOUND;
        }
    } else {
        release_dns_query(query);
    }
    release_dns_query(query);
    return result;
}

static int connect_wire(esp_transport_handle_t transport, const char *host, int port, int timeout)
{
    (void) timeout;
    firebase_https_connection_t *wire = esp_transport_get_context_data(transport);
    char address[IPADDR_STRLEN_MAX];
    wire->connect_error = resolve_address(wire, host, address);
    if (wire->connect_error != ESP_OK) return -1;
    wire->tls = esp_tls_init();
    if (!wire->tls) {
        wire->connect_error = ESP_ERR_NO_MEM;
        return -1;
    }
    /* Use the resolved address for the socket and the original hostname for
     * SNI and certificate verification. */
    tls_keep_alive_cfg_t keep_alive = {
        .keep_alive_enable = true, .keep_alive_idle = 5, .keep_alive_interval = 5, .keep_alive_count = 3,
    };
    esp_tls_cfg_t config = {
        .non_block = true, .common_name = host, .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_cfg = wire->keep_alive ? &keep_alive : NULL,
    };
    int left;
    while ((left = remaining_ms(wire)) > 0) {
        config.timeout_ms = left;
        const int connected = esp_tls_conn_new_async(address, (int) strlen(address), port, &config, wire->tls);
        if (connected != 0) return connected == 1 && remaining_ms(wire) > 0 ? 0 : -1;
        vTaskDelay(1);
    }
    return -1;
}

static bool retry_tls(int result)
{
    return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE;
}

/* HTTP can call these repeatedly inside one header or body read. Every
 * transport call consumes the same budget, including partial TLS records. */
static int read_wire(esp_transport_handle_t transport, char *buffer, int length, int timeout)
{
    (void) timeout;
    const firebase_https_connection_t *wire = esp_transport_get_context_data(transport);
    while (remaining_ms(wire) > 0) {
        const int result = esp_tls_conn_read(wire->tls, buffer, (size_t) length);
        if (result > 0) return result;
        if (!result) return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
        if (!retry_tls(result)) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
        vTaskDelay(1);
    }
    return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
}

static int write_wire(esp_transport_handle_t transport, const char *buffer, int length, int timeout)
{
    (void) timeout;
    const firebase_https_connection_t *wire = esp_transport_get_context_data(transport);
    while (remaining_ms(wire) > 0) {
        const int result = esp_tls_conn_write(wire->tls, buffer, (size_t) length);
        if (result >= 0) return result;
        if (!retry_tls(result)) return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
        vTaskDelay(1);
    }
    return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
}

static int close_wire(esp_transport_handle_t transport)
{
    firebase_https_connection_t *wire = esp_transport_get_context_data(transport);
    if (wire->tls) {
        esp_tls_conn_destroy(wire->tls);
        wire->tls = NULL;
    }
    return 0;
}

esp_err_t firebase_https_init(firebase_https_connection_t *connection,
                             esp_http_client_method_t method, const char *url, int budget_ms)
{
    *connection = (firebase_https_connection_t) {
        .deadline_us = esp_timer_get_time() + budget_ms * 1000LL,
        .keep_alive = method == HTTP_METHOD_GET,
    };
    esp_transport_handle_t transport = esp_transport_init();
    if (!transport) return ESP_ERR_NO_MEM;
    connection->transport = transport;
    esp_transport_set_context_data(transport, connection);
    esp_transport_set_default_port(transport, 443);
    esp_transport_set_func(transport, connect_wire, read_wire, write_wire, close_wire, NULL, NULL, close_wire);
    const esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .transport = transport,
        .timeout_ms = budget_ms,
        .buffer_size_tx = FIREBASE_HTTP_TX_BUFFER_BYTES,
        .disable_auto_redirect = true,
    };
    connection->http = esp_http_client_init(&config);
    if (!connection->http) {
        firebase_https_cleanup(connection);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void firebase_https_cleanup(firebase_https_connection_t *connection)
{
    if (connection->http) esp_http_client_cleanup(connection->http);
    if (connection->transport) esp_transport_destroy(connection->transport);
}

esp_err_t firebase_https_request(esp_http_client_method_t method, const char *url,
                                const char *content_type, const char *body,
                                char *response, size_t capacity, int *status)
{
    *status = 0;
    if (response && capacity) response[0] = '\0';
    firebase_https_connection_t wire;
    esp_err_t err = firebase_https_init(&wire, method, url, REQUEST_BUDGET_MS);
    if (err != ESP_OK) return err;
    esp_http_client_handle_t client = wire.http;
    err = esp_http_client_set_header(client, "Content-Type", content_type);
    const size_t length = body ? strlen(body) : 0;
    if (err == ESP_OK) err = esp_http_client_open(client, (int) length);
    size_t sent = 0;
    while (err == ESP_OK && sent < length) {
        if (!remaining_ms(&wire)) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        const int count = esp_http_client_write(client, body + sent, (int) (length - sent));
        if (count <= 0) {
            err = ESP_FAIL;
            break;
        }
        sent += (size_t) count;
    }

    if (err == ESP_OK) {
        const int64_t expected = esp_http_client_fetch_headers(client);
        *status = esp_http_client_get_status_code(client);
        if (expected < 0 || *status < 100) err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK && response && capacity && *status != 204 && *status != 202) {
        size_t used = 0;
        for (;;) {
            if (!remaining_ms(&wire)) { err = ESP_ERR_TIMEOUT; break; }
            /* fetch_headers may already have received and cached the body. */
            char overflow;
            const size_t room = capacity - 1 - used;
            const int count = esp_http_client_read(client, room ? response + used : &overflow,
                                                   room ? (int) room : 1);
            if (count == 0 && esp_http_client_is_complete_data_received(client)) break;
            if (count <= 0) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            if (!room) { err = ESP_ERR_INVALID_SIZE; break; }
            if (memchr(response + used, '\0', (size_t) count)) { err = ESP_ERR_INVALID_RESPONSE; break; }
            used += (size_t) count;
        }
        response[used] = '\0';
    }
    if (!remaining_ms(&wire)) err = ESP_ERR_TIMEOUT;
    else if (wire.connect_error != ESP_OK) err = wire.connect_error;
    firebase_https_cleanup(&wire);
    return err;
}

#endif
