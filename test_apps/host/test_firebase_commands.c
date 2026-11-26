#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "app_runtime.h"
#include "clock_service.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "firebase_commands.h"
#include "firebase_https.h"
#include "firmware_update.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "printer_control.h"
#include "wifi_manager.h"

#define TEST_WAIT_MS 4000U

typedef struct { char bytes[256]; } stream_event_t;
static const stream_event_t empty_snapshot = {"event: put\ndata: {\"path\":\"/\",\"data\":null}\n\n"};
static QueueHandle_t incoming_events;
static SemaphoreHandle_t target_started, finish_target, disable_started, finish_disable, enable_started;
static SemaphoreHandle_t enable_waiting, finish_enable;
static SemaphoreHandle_t monitor_stop_waiting, finish_monitor_stop;
static SemaphoreHandle_t wifi_lock;
static atomic_bool hold_enable;
static atomic_bool relay_on = true;
static atomic_bool wifi_connected = true, clock_recent = true;
static atomic_uint wifi_generation;
static atomic_uint enables;
static atomic_uint stream_opens;
static atomic_uint update_requests;
static bool delay_first_headers;
static const app_runtime_enable_guard_t *relay_guard;

static uint64_t monotonic_ms(void) { return (uint64_t) esp_timer_get_time() / 1000U; }

esp_err_t firmware_update_request(void)
{
    atomic_fetch_add(&update_requests, 1U);
    return ESP_OK;
}

void app_runtime_set_relay_start_guard(const app_runtime_enable_guard_t *guard) { relay_guard = guard; }

esp_err_t app_runtime_set_target_until(float target, uint64_t deadline)
{
    assert(target == 55.0f && monotonic_ms() < deadline);
    xSemaphoreGive(target_started);
    assert(xSemaphoreTake(finish_target, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    return ESP_OK;
}

esp_err_t app_runtime_set_restart_until(float restart, uint64_t deadline)
{
    assert(restart == 55.5f && monotonic_ms() < deadline);
    return ESP_OK;
}

esp_err_t app_runtime_set_enabled_until(bool enabled, uint64_t deadline)
{
    assert(!enabled && monotonic_ms() < deadline);
    atomic_store(&relay_on, false);
    xSemaphoreGive(disable_started);
    assert(xSemaphoreTake(finish_disable, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    return ESP_OK;
}

esp_err_t app_runtime_enable_if_current(uint64_t deadline, const app_runtime_enable_guard_t *guard,
                                        uint32_t revision)
{
    assert(monotonic_ms() < deadline);
    if (atomic_load(&hold_enable)) {
        xSemaphoreGive(enable_waiting);
        assert(xSemaphoreTake(finish_enable, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    }
    if (!guard->lock_if_current(revision)) return ESP_ERR_NOT_ALLOWED;
    assert(xSemaphoreTake(wifi_lock, 0) == pdFALSE);
    atomic_store(&relay_on, true);
    guard->unlock();
    atomic_fetch_add(&enables, 1U);
    xSemaphoreGive(enable_started);
    return ESP_OK;
}

esp_err_t app_runtime_set_calibration_until(float offset, uint64_t deadline)
{ (void) offset; (void) deadline; return ESP_ERR_NOT_ALLOWED; }
esp_err_t app_runtime_ack_fault_until(uint64_t deadline)
{ (void) deadline; return ESP_ERR_NOT_ALLOWED; }
esp_err_t printer_control_monitor_start_until(uint64_t deadline)
{ (void) deadline; return ESP_ERR_NOT_ALLOWED; }
esp_err_t printer_control_monitor_stop_until(uint64_t deadline,
    const app_runtime_enable_guard_t *guard, uint32_t revision)
{
    assert(monotonic_ms() < deadline);
    xSemaphoreGive(monitor_stop_waiting);
    assert(xSemaphoreTake(finish_monitor_stop, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    if (guard && !guard->lock_if_current(revision)) return ESP_ERR_NOT_ALLOWED;
    atomic_store(&relay_on, true);
    if (guard) guard->unlock();
    return ESP_OK;
}
bool wifi_manager_lock_connection(uint32_t *generation)
{
    assert(xSemaphoreTake(wifi_lock, portMAX_DELAY) == pdTRUE);
    if (!atomic_load(&wifi_connected)) {
        xSemaphoreGive(wifi_lock);
        return false;
    }
    *generation = atomic_load(&wifi_generation);
    return true;
}
void wifi_manager_unlock_connection(void) { xSemaphoreGive(wifi_lock); }
bool wifi_manager_connection_generation(uint32_t *generation)
{
    if (!wifi_manager_lock_connection(generation)) return false;
    wifi_manager_unlock_connection();
    return true;
}
uint32_t wifi_manager_backoff_ms(size_t attempt) { (void) attempt; return 100U; }
bool clock_service_is_recent(uint32_t max_age_seconds)
{ return max_age_seconds > 0 && atomic_load(&clock_recent); }
void esp_fill_random(void *out, size_t length)
{
    static unsigned session_seed;
    memset(out, (int) ++session_seed, length);
}

static struct http_fixture { firebase_https_connection_t *connection; } http;
esp_err_t firebase_https_init(firebase_https_connection_t *connection,
                             esp_http_client_method_t method, const char *url, int budget_ms)
{
    assert(method == HTTP_METHOD_GET && strstr(url, "/commands.json?auth=") && budget_ms == 6000);
    *connection = (firebase_https_connection_t) {
        .http = &http, .deadline_us = esp_timer_get_time() + budget_ms * 1000LL,
    };
    http.connection = connection;
    return ESP_OK;
}
void firebase_https_cleanup(firebase_https_connection_t *connection) { assert(connection == http.connection); }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value)
{
    assert(client == &http && strcmp(name, "Accept") == 0 && strcmp(value, "text/event-stream") == 0);
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int length)
{
    assert(client == &http && length == 0);
    atomic_fetch_add(&stream_opens, 1U);
    return ESP_OK;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    assert(client == &http);
    if (delay_first_headers && atomic_load(&stream_opens) == 1U) vTaskDelay(pdMS_TO_TICKS(4000));
    return 0;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client)
{ assert(client == &http); return 200; }
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client)
{ (void) client; return ESP_ERR_NOT_ALLOWED; }
esp_err_t esp_http_client_get_url(esp_http_client_handle_t client, char *url, int capacity)
{ (void) client; (void) url; (void) capacity; return ESP_ERR_NOT_ALLOWED; }
esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{ assert(client == &http); return ESP_OK; }
int esp_http_client_read(esp_http_client_handle_t client, char *body, int capacity)
{
    stream_event_t event;
    const int64_t remaining = (client->connection->deadline_us - esp_timer_get_time()) / 1000;
    if (remaining <= 0 ||
        xQueueReceive(incoming_events, &event, pdMS_TO_TICKS((uint32_t) remaining)) != pdTRUE) {
        return -ESP_ERR_HTTP_EAGAIN;
    }
    const size_t length = strlen(event.bytes);
    assert(length <= (size_t) capacity);
    memcpy(body, event.bytes, length);
    return (int) length;
}

static firebase_command_status_t wait_for_activity(uint64_t previous_ms)
{
    const uint64_t deadline = monotonic_ms() + TEST_WAIT_MS;
    firebase_command_status_t channel;
    do {
        firebase_commands_get_status(&channel);
        if (channel.ready && channel.last_activity_ms > previous_ms) return channel;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (monotonic_ms() < deadline);
    assert(!"stream did not report activity");
    return channel;
}

static firebase_command_status_t wait_for_session(const char *previous_session)
{
    const uint64_t deadline = monotonic_ms() + TEST_WAIT_MS;
    firebase_command_status_t channel;
    do {
        firebase_commands_get_status(&channel);
        if (channel.ready && strcmp(channel.session_id, previous_session) != 0) return channel;
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (monotonic_ms() < deadline);
    assert(!"stream did not publish a new session");
    return channel;
}

static void send_command(const char *session, const char *id, unsigned sequence, const char *type,
                         const char *payload, uint64_t lifetime_ms)
{
    struct timeval time;
    assert(gettimeofday(&time, NULL) == 0);
    const uint64_t issued = (uint64_t) time.tv_sec * 1000U + (uint64_t) time.tv_usec / 1000U;
    stream_event_t event;
    const int length = snprintf(event.bytes, sizeof(event.bytes),
        "event: put\ndata: {\"path\":\"/%s\",\"data\":{\"id\":\"%s\",\"sequence\":%u,"
        "\"sessionId\":\"%s\",\"type\":\"%s\",\"issuedAt\":%" PRIu64 ",\"expiresAt\":%" PRIu64 "%s}}\n\n",
        id, id, sequence, session, type, issued, issued + lifetime_ms, payload);
    assert(length > 0 && (size_t) length < sizeof(event.bytes));
    assert(xQueueSend(incoming_events, &event, 0) == pdTRUE);
}

static void expect_result(const char *id, const char *status)
{
    const uint64_t deadline = monotonic_ms() + TEST_WAIT_MS;
    firebase_command_result_t result;
    while (!firebase_commands_peek_result(&result)) {
        assert(monotonic_ms() < deadline);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    assert(strcmp(result.id, id) == 0 && strcmp(result.status, status) == 0);
    firebase_commands_remove_result();
}

static void commands_wait_for_result_space(const char *session)
{
    /* Fill the result queue to the slot reserved for Disable. */
    const unsigned backlog = 11;
    char id[24];
    for (unsigned i = 1; i <= backlog; ++i) {
        snprintf(id, sizeof(id), "queued-%u", i);
        send_command(session, id, i, "heat.enable", "", 30000);
        assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    }

    send_command(session, "expires-while-waiting", backlog + 1, "heat.enable", "", 200);
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(350)) == pdFALSE);
    send_command(session, "wait-for-result-room", backlog + 2, "heat.enable", "", 30000);
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(100)) == pdFALSE);

    /* Freeing a slot must wake the worker without another stream update. */
    expect_result("queued-1", "applied");
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);

    send_command(session, "superseded-while-waiting", backlog + 3, "heat.enable", "", 30000);
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(100)) == pdFALSE);
    send_command(session, "disable-during-backlog", backlog + 4, "heat.disable", "", 30000);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    assert(!atomic_load(&relay_on));
    xSemaphoreGive(finish_disable);

    for (unsigned i = 2; i <= backlog; ++i) {
        snprintf(id, sizeof(id), "queued-%u", i);
        expect_result(id, "applied");
    }
    expect_result("wait-for-result-room", "applied");
    expect_result("disable-during-backlog", "applied");
    assert(atomic_load(&enables) == backlog + 1);
    assert(!atomic_load(&relay_on));
}

static void monitor_stop_requires_current_session(const char *session)
{
    atomic_store(&relay_on, false);
    send_command(session, "valid-monitor-stop", 1, "printer.monitorStop", "", 30000);
    assert(xSemaphoreTake(monitor_stop_waiting, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    xSemaphoreGive(finish_monitor_stop);
    expect_result("valid-monitor-stop", "applied");
    assert(atomic_load(&relay_on));

    atomic_store(&relay_on, false);
    send_command(session, "stale-monitor-stop", 2, "printer.monitorStop", "", 30000);
    assert(xSemaphoreTake(monitor_stop_waiting, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    firebase_commands_set_auth(NULL, NULL);
    xSemaphoreGive(finish_monitor_stop);
    expect_result("stale-monitor-stop", "rejected");
    assert(!atomic_load(&relay_on));
}

static firebase_command_status_t renew_token(firebase_command_status_t old_channel, const char *token)
{
    firebase_commands_set_auth("test-device", token);
    /* Wake the old HTTP reader, then send the new stream's initial snapshot. */
    assert(xQueueSend(incoming_events, &empty_snapshot, 0) == pdTRUE);
    assert(xQueueSend(incoming_events, &empty_snapshot, 0) == pdTRUE);
    return wait_for_session(old_channel.session_id);
}

static void disable_survives_token_refresh(firebase_command_status_t original)
{
    const firebase_command_status_t renewed = renew_token(original, "renewed-token");
    send_command(original.session_id, "stale-enable", 1, "heat.enable", "", 30000);
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(100)) == pdFALSE);

    send_command(original.session_id, "disable-after-renewal", 2, "heat.disable", "", 30000);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    assert(!atomic_load(&relay_on));
    xSemaphoreGive(finish_disable);
    expect_result("disable-after-renewal", "applied");

    /* An in-flight Disable keeps its barrier while an Enable from the new session waits. */
    send_command(renewed.session_id, "disable-before-renewal", 3, "heat.disable", "", 30000);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    const firebase_command_status_t next = renew_token(renewed, "newer-token");
    const firebase_command_status_t current = renew_token(next, "latest-token");
    send_command(current.session_id, "current-enable", 4, "heat.enable", "", 30000);
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(100)) == pdFALSE);
    xSemaphoreGive(finish_disable);
    expect_result("disable-before-renewal", "applied");
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    expect_result("current-enable", "applied");
}

static firebase_command_status_t reconnect_stream(firebase_command_status_t previous)
{
    const stream_event_t end_of_stream = {""};
    assert(xQueueSend(incoming_events, &end_of_stream, 0) == pdTRUE);
    assert(xQueueSend(incoming_events, &empty_snapshot, 0) == pdTRUE);
    return wait_for_session(previous.session_id);
}

static void disable_survives_reconnect(firebase_command_status_t original, bool lose_wifi)
{
    const firebase_command_status_t reconnected = reconnect_stream(original);
    send_command(original.session_id, "stop-from-previous-stream", 1, "heat.disable", "", 30000);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    assert(!atomic_load(&relay_on));

    send_command(reconnected.session_id, "queued-stop", 2, "heat.disable", "", 30000);
    const firebase_command_status_t current = reconnect_stream(reconnected);
    /* The reader consumed queued-stop before the end of the previous stream. */
    if (lose_wifi) {
        atomic_store(&wifi_connected, false);
        atomic_store(&clock_recent, false);
        firebase_commands_set_auth("test-device", NULL);
    } else {
        send_command(current.session_id, "enable-after-stop", 3, "heat.enable", "", 30000);
        assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(100)) == pdFALSE);
    }
    xSemaphoreGive(finish_disable);
    expect_result("stop-from-previous-stream", "applied");
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    assert(!atomic_load(&relay_on));
    xSemaphoreGive(finish_disable);
    expect_result("queued-stop", "applied");
    if (!lose_wifi) {
        assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
        expect_result("enable-after-stop", "applied");
    }
}

static void disable_survives_repeated_reconnects(firebase_command_status_t advertised)
{
    const firebase_command_status_t next = reconnect_stream(advertised);
    const firebase_command_status_t current = reconnect_stream(next);
    /* Telemetry still shows the original session. Its Disable remains eligible. */
    char future_session[FIREBASE_COMMAND_SESSION_BYTES];
    strcpy(future_session, current.session_id);
    strcpy(future_session + FIREBASE_COMMAND_SESSION_PREFIX_LEN, "ffffffff");
    send_command(future_session, "future-stop", 4, "heat.disable", "", 30000);
    send_command(advertised.session_id, "superseded-enable", 2, "heat.enable", "", 30000);
    send_command(advertised.session_id, "advertised-stop", 1, "heat.disable", "", 30000);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    assert(!atomic_load(&relay_on) && atomic_load(&enables) == 0);
    xSemaphoreGive(finish_disable);
    expect_result("advertised-stop", "applied");

    (void) reconnect_stream(current);
    send_command(advertised.session_id, "advertised-stop", 1, "heat.disable", "", 30000);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(100)) == pdFALSE);
}

static void boot_snapshot_uses_previous_boot_disable(void)
{
    struct timeval time;
    assert(gettimeofday(&time, NULL) == 0);
    const uint64_t issued = (uint64_t) time.tv_sec * 1000U + (uint64_t) time.tv_usec / 1000U;
    stream_event_t event;
    const int length = snprintf(event.bytes, sizeof(event.bytes),
        "event: put\ndata: {\"path\":\"/\",\"data\":{\"old-stop\":{\"id\":\"old-stop\",\"sequence\":1,"
        "\"sessionId\":\"ffffffffffffffffffffffff00000000\",\"type\":\"heat.disable\","
        "\"issuedAt\":%" PRIu64 ",\"expiresAt\":%" PRIu64 "}}}\n\n", issued, issued + 30000U);
    assert(length > 0 && (size_t) length < sizeof(event.bytes));
    assert(xQueueSend(incoming_events, &event, 0) == pdTRUE);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    assert(!atomic_load(&relay_on));
    assert(relay_guard && !relay_guard->lock_relay_start());
    xSemaphoreGive(finish_disable);
    expect_result("old-stop", "applied");
    const uint64_t deadline = monotonic_ms() + TEST_WAIT_MS;
    while (!relay_guard->lock_relay_start()) {
        assert(monotonic_ms() < deadline);
        vTaskDelay(1);
    }
    relay_guard->unlock_relay_start();
}

static void disable_reconnects_after_initial_snapshot_timeout(void)
{
    /* The first response has slow headers and begins a snapshot that never finishes. */
    const stream_event_t partial = {"event: put\ndata: {\"path\":"};
    assert(xQueueSend(incoming_events, &partial, 0) == pdTRUE);
    const uint64_t deadline = monotonic_ms() + 8500U;
    while (atomic_load(&stream_opens) < 2U && monotonic_ms() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    assert(atomic_load(&stream_opens) == 2U);
    boot_snapshot_uses_previous_boot_disable();
}

static void readings_only_accepts_disable(const char *session)
{
    atomic_store(&clock_recent, false);
    firebase_commands_set_auth("test-device", "id-token");
    firebase_command_status_t status;
    firebase_commands_get_status(&status);
    assert(!status.ready);
    send_command(session, "blocked-enable", 1, "heat.enable", "", 30000);
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(200)) == pdFALSE);
    send_command("00000000000000000000000000000000", "readings-stop", 2, "heat.disable", "", 30000);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    assert(!atomic_load(&relay_on));
    xSemaphoreGive(finish_disable);
    expect_result("readings-stop", "applied");
}

static void pending_enable_requires_current_wifi(const char *session, bool reconnected)
{
    atomic_store(&relay_on, false);
    atomic_store(&hold_enable, true);
    send_command(session, "pending-enable", 1, "heat.enable", "", 30000);
    assert(xSemaphoreTake(enable_waiting, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);

    assert(xSemaphoreTake(wifi_lock, portMAX_DELAY) == pdTRUE);
    atomic_store(&wifi_connected, false);
    atomic_fetch_add(&wifi_generation, 1U);
    if (reconnected) atomic_store(&wifi_connected, true);
    xSemaphoreGive(wifi_lock);
    xSemaphoreGive(finish_enable);
    expect_result("pending-enable", "rejected");
    assert(!atomic_load(&relay_on) && atomic_load(&enables) == 0);
}

int main(int argc, char **argv)
{
    incoming_events = xQueueCreate(8, sizeof(stream_event_t));
    target_started = xSemaphoreCreateBinary();
    finish_target = xSemaphoreCreateBinary();
    disable_started = xSemaphoreCreateBinary();
    finish_disable = xSemaphoreCreateBinary();
    enable_started = xSemaphoreCreateBinary();
    enable_waiting = xSemaphoreCreateBinary();
    finish_enable = xSemaphoreCreateBinary();
    monitor_stop_waiting = xSemaphoreCreateBinary();
    finish_monitor_stop = xSemaphoreCreateBinary();
    wifi_lock = xSemaphoreCreateMutex();
    assert(incoming_events && target_started && finish_target && disable_started && finish_disable && enable_started);
    assert(enable_waiting && finish_enable);
    assert(monitor_stop_waiting && finish_monitor_stop && wifi_lock);
    delay_first_headers = argc == 2 && strcmp(argv[1], "initial_snapshot_timeout") == 0;
    assert(firebase_commands_start() == ESP_OK);
    firebase_commands_set_auth("test-device", "id-token");
    if (delay_first_headers) {
        disable_reconnects_after_initial_snapshot_timeout();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "boot_stop") == 0) {
        boot_snapshot_uses_previous_boot_disable();
        return 0;
    }
    assert(xQueueSend(incoming_events, &empty_snapshot, 0) == pdTRUE);
    const firebase_command_status_t channel = wait_for_session("");

    if (argc == 2) {
        if (strcmp(argv[1], "backpressure") == 0) commands_wait_for_result_space(channel.session_id);
        else if (strcmp(argv[1], "monitor_stop_session") == 0) monitor_stop_requires_current_session(channel.session_id);
        else if (strcmp(argv[1], "token_renewal") == 0) disable_survives_token_refresh(channel);
        else if (strcmp(argv[1], "stream_reconnect") == 0) disable_survives_reconnect(channel, false);
        else if (strcmp(argv[1], "wifi_disconnect") == 0) disable_survives_reconnect(channel, true);
        else if (strcmp(argv[1], "repeated_reconnects") == 0) disable_survives_repeated_reconnects(channel);
        else if (strcmp(argv[1], "readings_only") == 0) readings_only_accepts_disable(channel.session_id);
        else if (strcmp(argv[1], "enable_wifi_loss") == 0) pending_enable_requires_current_wifi(channel.session_id, false);
        else if (strcmp(argv[1], "enable_wifi_reconnect") == 0) pending_enable_requires_current_wifi(channel.session_id, true);
        else if (strcmp(argv[1], "restart_setting") == 0) {
            send_command(channel.session_id, "restart", 1, "heat.setRestart", ",\"payload\":{\"restartC\":55.5}", 30000);
            expect_result("restart", "applied");
        }
        else if (strcmp(argv[1], "firmware_update") == 0) {
            send_command(channel.session_id, "ota", 1, "firmware.update", "", 30000);
            expect_result("ota", "applied");
            assert(atomic_load(&update_requests) == 1);
        }
        else assert(!"unknown Firebase command scenario");
        return 0;
    }

    send_command(channel.session_id, "target", 1, "heat.setTarget", ",\"payload\":{\"targetC\":55}", 30000);
    assert(xSemaphoreTake(target_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    send_command(channel.session_id, "old-enable", 2, "heat.enable", "", 30000);
    send_command(channel.session_id, "disable", 3, "heat.disable", "", 30000);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    assert(!atomic_load(&relay_on));

    /* Both workers are waiting on persistence. The reader must still make progress. */
    firebase_command_status_t before_heartbeat;
    firebase_commands_get_status(&before_heartbeat);
    assert(before_heartbeat.ready);
    vTaskDelay(pdMS_TO_TICKS(100));
    const stream_event_t heartbeat = {"event: keep-alive\ndata: null\n\n"};
    assert(xQueueSend(incoming_events, &heartbeat, 0) == pdTRUE);
    (void) wait_for_activity(before_heartbeat.last_activity_ms);

    send_command(channel.session_id, "expired-enable", 4, "heat.enable", "", 200);
    send_command(channel.session_id, "fresh-enable", 5, "heat.enable", "", 30000);
    xSemaphoreGive(finish_target);
    expect_result("target", "applied");
    expect_result("expired-enable", "expired");
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(200)) == pdFALSE);
    assert(!atomic_load(&relay_on));
    xSemaphoreGive(finish_disable);
    expect_result("disable", "applied");
    assert(xSemaphoreTake(enable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    expect_result("fresh-enable", "applied");
    assert(atomic_load(&relay_on) && atomic_load(&enables) == 1U);

    /* Disable can run while an Enable is already waiting in the runtime. */
    atomic_store(&hold_enable, true);
    send_command(channel.session_id, "waiting-enable", 6, "heat.enable", "", 30000);
    assert(xSemaphoreTake(enable_waiting, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    send_command(channel.session_id, "urgent-disable", 7, "heat.disable", "", 30000);
    assert(xSemaphoreTake(disable_started, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    assert(!atomic_load(&relay_on));
    xSemaphoreGive(finish_disable);
    expect_result("urgent-disable", "applied");
    xSemaphoreGive(finish_enable);
    expect_result("waiting-enable", "rejected");

    send_command(channel.session_id, "session-enable", 8, "heat.enable", "", 30000);
    assert(xSemaphoreTake(enable_waiting, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    firebase_commands_set_auth(NULL, NULL);
    xSemaphoreGive(finish_enable);
    expect_result("session-enable", "rejected");
    assert(!atomic_load(&relay_on) && atomic_load(&enables) == 1U);
    puts("Firebase command priority and stream activity passed");
    return 0;
}
