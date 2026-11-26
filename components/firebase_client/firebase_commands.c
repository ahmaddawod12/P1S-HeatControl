#include "firebase_commands.h"

#include "sdkconfig.h"

#ifdef CONFIG_P1S_FIREBASE_ENABLED

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "app_runtime.h"
#include "clock_service.h"
#include "device_config.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "firebase_https.h"
#include "firmware_update.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "printer_control.h"
#include "wifi_manager.h"

#define COMMAND_CLOCK_MAX_AGE_SECONDS (30U * 60U)
#define STREAM_STARTUP_TIMEOUT_MS 6000
#define STREAM_READ_TIMEOUT_MS 1500
#define STREAM_IDLE_TIMEOUT_MS 60000U
#define STREAM_TASK_STACK 8192U
#define COMMAND_TASK_STACK 4096U
#define DISABLE_TASK_STACK 3072U
#define COMMAND_QUEUE_DEPTH 8U
#define RESULT_QUEUE_DEPTH 12U
/* The Firebase backend retains a command receipt for up to 24 hours. */
#define RESULT_RETRY_WINDOW_MS (24ULL * 60U * 60U * 1000U)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_uid[FIREBASE_UID_BYTES], s_token[FIREBASE_ID_TOKEN_BYTES];
static unsigned s_generation;
static firebase_command_status_t s_status;
static uint32_t s_channel_wifi_generation;
/* Guarded by s_lock. The stream publishes the barrier before queuing Disable. */
static uint32_t s_disable_sequence;
static bool s_disable_pending;
static char s_pending_disable_session[FIREBASE_COMMAND_SESSION_BYTES];
static uint32_t s_enable_revision;
/* Serializes revision changes with relay activation or release of a monitor
 * hold. Release it before calling the runtime or waiting for persistence. */
static SemaphoreHandle_t s_enable_commit_lock;
static StaticSemaphore_t s_enable_commit_lock_storage;
static QueueHandle_t s_commands, s_disable, s_results;
static TaskHandle_t s_stream_task, s_command_task, s_disable_task;

typedef struct {
    firebase_command_t command;
    char session_id[FIREBASE_COMMAND_SESSION_BYTES];
    uint64_t deadline_ms;
    bool invalid_clock;
} queued_command_t;

typedef struct {
    unsigned generation;
    uint32_t wifi_generation, next_session_number;
    bool ready;
    uint64_t last_activity_ms;
    uint32_t last_sequence;
    char seen[FIREBASE_COMMAND_BATCH_MAX][FIREBASE_COMMAND_ID_BYTES];
    size_t seen_index;
    char uid[FIREBASE_UID_BYTES];
    char token[FIREBASE_ID_TOKEN_BYTES];
    char session_id[FIREBASE_COMMAND_SESSION_BYTES];
    char url[DEVICE_FIREBASE_DATABASE_BYTES + FIREBASE_UID_BYTES + FIREBASE_ID_TOKEN_BYTES + 128];
    firebase_sse_t parser;
    firebase_command_t commands[FIREBASE_COMMAND_BATCH_MAX];
} stream_context_t;

static uint64_t monotonic_ms(void)
{
    return (uint64_t) esp_timer_get_time() / 1000U;
}

static uint64_t utc_ms(void)
{
    struct timeval time;
    if (gettimeofday(&time, NULL) != 0 || time.tv_sec < 0) return 0;
    return (uint64_t) time.tv_sec * 1000U + (uint64_t) time.tv_usec / 1000U;
}

static bool activity_expired(uint64_t activity_ms, uint64_t now)
{
    return now >= activity_ms && now - activity_ms >= STREAM_IDLE_TIMEOUT_MS;
}

static bool command_wifi_generation(uint32_t *generation)
{
    return clock_service_is_recent(COMMAND_CLOCK_MAX_AGE_SECONDS) &&
        wifi_manager_connection_generation(generation);
}

/* Caller holds s_enable_commit_lock and s_lock. */
static void clear_channel(bool same_uid)
{
    if (!same_uid) {
        s_disable_sequence = 0;
        s_disable_pending = false;
    }
    s_status = (firebase_command_status_t) {0};
    ++s_enable_revision;
}

void firebase_commands_set_auth(const char *uid, const char *id_token)
{
    if (!uid) uid = "";
    if (!uid[0] || !id_token) {
        id_token = "";
    }
    xSemaphoreTake(s_enable_commit_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    const bool same_uid = strcmp(s_uid, uid) == 0;
    const bool changed = !same_uid || strcmp(s_token, id_token) != 0;
    if (changed) {
        clear_channel(same_uid);
        strcpy(s_uid, uid);
        strcpy(s_token, id_token);
        ++s_generation;
    }
    taskEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_enable_commit_lock);
    if (changed) {
        if (s_stream_task) xTaskNotifyGive(s_stream_task);
        if (s_command_task) xTaskNotifyGive(s_command_task);
    }
}

void firebase_commands_get_status(firebase_command_status_t *out)
{
    uint32_t wifi_generation;
    const bool ready = command_wifi_generation(&wifi_generation);
    const uint64_t now = monotonic_ms();
    taskENTER_CRITICAL(&s_lock);
    *out = ready && wifi_generation == s_channel_wifi_generation &&
        !activity_expired(s_status.last_activity_ms, now)
        ? s_status : (firebase_command_status_t) {0};
    taskEXIT_CRITICAL(&s_lock);
}

bool firebase_commands_peek_result(firebase_command_result_t *out)
{
    return xQueuePeek(s_results, out, 0) == pdTRUE;
}

void firebase_commands_remove_result(void)
{
    firebase_command_result_t result;
    if (xQueueReceive(s_results, &result, 0) == pdTRUE) xTaskNotifyGive(s_command_task);
}

static bool stream_current(const stream_context_t *stream)
{
    uint32_t wifi_generation;
    if (!wifi_manager_connection_generation(&wifi_generation) || wifi_generation != stream->wifi_generation) return false;
    taskENTER_CRITICAL(&s_lock);
    const bool current = stream->generation == s_generation;
    taskEXIT_CRITICAL(&s_lock);
    return current;
}

static void record_activity(stream_context_t *stream, uint64_t now)
{
    stream->ready = true;
    stream->last_activity_ms = now;
    taskENTER_CRITICAL(&s_lock);
    if (stream->generation == s_generation) {
        s_channel_wifi_generation = stream->wifi_generation;
        s_status.ready = true;
        s_status.last_activity_ms = now;
        strcpy(s_status.session_id, stream->session_id);
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void close_channel(stream_context_t *stream)
{
    xSemaphoreTake(s_enable_commit_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    if (stream->generation == s_generation) clear_channel(true);
    taskEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_enable_commit_lock);
    xQueueReset(s_commands);
    xTaskNotifyGive(s_command_task);
    stream->ready = false;
}

static void new_session(stream_context_t *stream)
{
    /* Keep the boot prefix so unexpired Disable requests can use a session
     * still advertised by telemetry, even if several streams have closed. */
    const size_t prefix = FIREBASE_COMMAND_SESSION_PREFIX_LEN;
    if (!stream->next_session_number) {
        static const char hex[] = "0123456789abcdef";
        uint8_t entropy[FIREBASE_COMMAND_SESSION_PREFIX_LEN / 2U];
        esp_fill_random(entropy, sizeof(entropy));
        for (size_t i = 0; i < sizeof(entropy); ++i) {
            stream->session_id[2 * i] = hex[entropy[i] >> 4U];
            stream->session_id[2 * i + 1] = hex[entropy[i] & 15U];
        }
    }
    snprintf(stream->session_id + prefix, sizeof(stream->session_id) - prefix,
             "%08" PRIx32, stream->next_session_number++);
}

static void unlock_enable(void)
{
    wifi_manager_unlock_connection();
    xSemaphoreGive(s_enable_commit_lock);
}

/* This also runs in the runtime, after the command leaves our queue. */
static bool lock_enable_if_current(uint32_t revision)
{
    xSemaphoreTake(s_enable_commit_lock, portMAX_DELAY);
    uint32_t wifi_generation;
    if (!clock_service_is_recent(COMMAND_CLOCK_MAX_AGE_SECONDS) ||
        !wifi_manager_lock_connection(&wifi_generation)) {
        xSemaphoreGive(s_enable_commit_lock);
        return false;
    }
    const uint64_t now = monotonic_ms();
    taskENTER_CRITICAL(&s_lock);
    const bool current = wifi_generation == s_channel_wifi_generation &&
        revision == s_enable_revision && !s_disable_pending &&
        s_status.ready && !activity_expired(s_status.last_activity_ms, now);
    taskEXIT_CRITICAL(&s_lock);
    if (!current) unlock_enable();
    return current;
}

static bool lock_relay_start(void)
{
    xSemaphoreTake(s_enable_commit_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    const bool allowed = !s_disable_pending;
    taskEXIT_CRITICAL(&s_lock);
    if (!allowed) xSemaphoreGive(s_enable_commit_lock);
    return allowed;
}

static void unlock_relay_start(void)
{
    xSemaphoreGive(s_enable_commit_lock);
}

static const app_runtime_enable_guard_t s_enable_guard = {
    .lock_if_current = lock_enable_if_current,
    .unlock = unlock_enable,
    .lock_relay_start = lock_relay_start,
    .unlock_relay_start = unlock_relay_start,
};

static esp_err_t run_command(const firebase_command_t *command, uint64_t deadline, uint32_t enable_revision)
{
    switch (command->type) {
    case FIREBASE_COMMAND_SET_TARGET: return app_runtime_set_target_until(command->value, deadline);
    case FIREBASE_COMMAND_SET_RESTART: return app_runtime_set_restart_until(command->value, deadline);
    case FIREBASE_COMMAND_ENABLE:
        return app_runtime_enable_if_current(deadline, &s_enable_guard, enable_revision);
    case FIREBASE_COMMAND_DISABLE: return app_runtime_set_enabled_until(false, deadline);
    case FIREBASE_COMMAND_ACK_FAULT: return app_runtime_ack_fault_until(deadline);
    case FIREBASE_COMMAND_CALIBRATE: return app_runtime_set_calibration_until(command->value, deadline);
    case FIREBASE_COMMAND_MONITOR_START: return printer_control_monitor_start_until(deadline);
    case FIREBASE_COMMAND_MONITOR_STOP:
        return printer_control_monitor_stop_until(deadline, &s_enable_guard, enable_revision);
    case FIREBASE_COMMAND_FIRMWARE_UPDATE:
#ifdef CONFIG_P1S_FIREBASE_PUBLIC_RELEASE
        if (monotonic_ms() >= deadline) return ESP_ERR_TIMEOUT;
        return firmware_update_request();
#else
        return ESP_ERR_NOT_ALLOWED;
#endif
    }
    return ESP_ERR_INVALID_ARG;
}

typedef enum {
    COMMAND_OBSOLETE,
    COMMAND_WAIT_FOR_DISABLE,
    COMMAND_READY,
} command_turn_t;

/* Caller holds s_lock. A received Disable uses its original deadline across reconnects. */
static bool pending_disable_matches(const queued_command_t *queued)
{
    return s_disable_pending && queued->command.sequence == s_disable_sequence &&
        strcmp(queued->session_id, s_pending_disable_session) == 0;
}

static command_turn_t command_turn(const queued_command_t *queued, char *uid, uint32_t *enable_revision)
{
    const bool disable = queued->command.type == FIREBASE_COMMAND_DISABLE;
    uint32_t wifi_generation = 0;
    if (!disable && !command_wifi_generation(&wifi_generation)) return COMMAND_OBSOLETE;
    const uint64_t now = monotonic_ms();
    taskENTER_CRITICAL(&s_lock);
    const bool current = disable ? pending_disable_matches(queued) :
        wifi_generation == s_channel_wifi_generation && queued->command.sequence > s_disable_sequence &&
        s_status.ready &&
        !activity_expired(s_status.last_activity_ms, now) && strcmp(queued->session_id, s_status.session_id) == 0;
    const bool wait = !disable && s_disable_pending;
    if (current && uid) strcpy(uid, s_uid);
    if (current && enable_revision) *enable_revision = s_enable_revision;
    taskEXIT_CRITICAL(&s_lock);
    return !current ? COMMAND_OBSOLETE : wait ? COMMAND_WAIT_FOR_DISABLE : COMMAND_READY;
}

static TickType_t ticks_until(uint64_t deadline_ms)
{
    const uint64_t now = monotonic_ms();
    if (now >= deadline_ms) return 0;
    const TickType_t ticks = pdMS_TO_TICKS(deadline_ms - now);
    return ticks ? ticks : 1U;
}

static void report_command(const queued_command_t *queued, firebase_command_result_t *result,
                           const char *status, const char *reason)
{
    strcpy(result->id, queued->command.id);
    result->deliver_until_ms = monotonic_ms() + RESULT_RETRY_WINDOW_MS;
    result->status = status;
    result->reason = reason;
    /* Multiple Disable results can use the available slots while a normal
     * command is in the runtime. Keep its outcome until it can be queued. */
    const bool disable = queued->command.type == FIREBASE_COMMAND_DISABLE;
    (void) xQueueSend(s_results, result, disable ? 0 : portMAX_DELAY);
}

static void apply_command(const queued_command_t *queued, firebase_command_result_t *result,
                          uint32_t enable_revision)
{
    const esp_err_t err = run_command(&queued->command, queued->deadline_ms, enable_revision);
    /* A timeout can race an applied command's confirmation. */
    if (err == ESP_ERR_TIMEOUT) return;
    const char *reason = err == ESP_OK ? NULL :
        err == ESP_ERR_INVALID_ARG ? "invalid_payload" :
        err == ESP_ERR_NOT_ALLOWED ? "precondition" : "controller_error";
    report_command(queued, result, err == ESP_OK ? "applied" : "rejected", reason);
}

static void execute_command(const queued_command_t *queued)
{
    const bool disable = queued->command.type == FIREBASE_COMMAND_DISABLE;
    for (;;) {
        const command_turn_t turn = command_turn(queued, NULL, NULL);
        if (turn == COMMAND_OBSOLETE) return;
        const TickType_t wait = ticks_until(queued->deadline_ms);
        /* Leave a result slot for Disable. Keep the command pending while
         * space is tight, up to its original deadline. */
        const bool can_report = disable || uxQueueSpacesAvailable(s_results) > 1;
        if (can_report && (turn == COMMAND_READY || !wait)) break;
        if (!wait) return;
        (void) ulTaskNotifyTake(pdTRUE, wait);
    }

    firebase_command_result_t result = {0};
    uint32_t enable_revision = 0;
    const command_turn_t turn = command_turn(queued, result.uid, &enable_revision);
    if (turn == COMMAND_OBSOLETE) return;
    if (queued->invalid_clock) {
        report_command(queued, &result, "rejected", "clock_unavailable");
    } else if (ticks_until(queued->deadline_ms) == 0) {
        report_command(queued, &result, "expired", "deadline");
    } else if (turn == COMMAND_READY) {
        apply_command(queued, &result, enable_revision);
    }
}

static void command_task(void *arg)
{
    const QueueHandle_t queue = arg;
    for (;;) {
        queued_command_t queued;
        if (xQueueReceive(queue, &queued, portMAX_DELAY) != pdTRUE) continue;
        execute_command(&queued);
        if (queue == s_disable) {
            taskENTER_CRITICAL(&s_lock);
            if (pending_disable_matches(&queued)) {
                s_disable_pending = false;
            }
            taskEXIT_CRITICAL(&s_lock);
            xTaskNotifyGive(s_command_task);
        }
    }
}

static bool queue_command(stream_context_t *stream, const firebase_command_t *command)
{
    if (command->sequence <= stream->last_sequence || !stream_current(stream)) return false;
    const bool disable = command->type == FIREBASE_COMMAND_DISABLE;
    if (!disable && !clock_service_is_recent(COMMAND_CLOCK_MAX_AGE_SECONDS)) return false;
    for (size_t i = 0; i < FIREBASE_COMMAND_BATCH_MAX; ++i) {
        if (strcmp(stream->seen[i], command->id) == 0) return false;
    }

    queued_command_t queued = {.command = *command};
    strcpy(queued.session_id, stream->session_id);
    const uint64_t wall = utc_ms();
    if (!firebase_command_deadline(command, wall, monotonic_ms(), &queued.deadline_ms)) {
        queued.invalid_clock = wall < command->expires_ms;
    }
    if (disable) {
        xSemaphoreTake(s_enable_commit_lock, portMAX_DELAY);
        taskENTER_CRITICAL(&s_lock);
        const bool current = stream->generation == s_generation && s_status.ready &&
            command->sequence > s_disable_sequence;
        if (current) {
            s_disable_sequence = command->sequence;
            s_disable_pending = true;
            strcpy(s_pending_disable_session, queued.session_id);
            ++s_enable_revision;
        }
        taskEXIT_CRITICAL(&s_lock);
        xSemaphoreGive(s_enable_commit_lock);
        if (!current) return false;
        /* Every queued command precedes this Disable. The worker rechecks
         * the barrier for any command it already took from the queue. */
        xQueueReset(s_commands);
        xQueueOverwrite(s_disable, &queued);
        xTaskNotifyGive(s_command_task);
    } else if (xQueueSend(s_commands, &queued, 0) != pdTRUE) {
        return false;
    }

    stream->last_sequence = command->sequence;
    strcpy(stream->seen[stream->seen_index], command->id);
    stream->seen_index = (stream->seen_index + 1) % FIREBASE_COMMAND_BATCH_MAX;
    return true;
}

static bool handle_event(const char *event, const char *data, void *context)
{
    stream_context_t *stream = context;
    const uint64_t now = monotonic_ms();
    if (!stream_current(stream) || activity_expired(stream->last_activity_ms, now) ||
        strcmp(event, "auth_revoked") == 0 || strcmp(event, "cancel") == 0) return false;
    const bool put = strcmp(event, "put") == 0;
    if (!put && strcmp(event, "patch") != 0) {
        if (stream->ready && strcmp(event, "keep-alive") == 0) record_activity(stream, now);
        return true;
    }

    bool root_snapshot;
    size_t count;
    if (!firebase_parse_command_batch(data, stream->session_id, &root_snapshot, stream->commands, &count)) return false;
    const bool initial = !stream->ready;
    if (initial && !(put && root_snapshot)) return false;
    record_activity(stream, now);
    /* A newer Disable in a batch invalidates older requests. Queue it
     * first to avoid energizing the relay for a superseded Enable. */
    const firebase_command_t *disable = NULL;
    for (size_t i = 0; i < count; ++i) {
        const firebase_command_t *command = &stream->commands[i];
        if (command->type == FIREBASE_COMMAND_DISABLE && (!disable || command->sequence > disable->sequence)) {
            disable = command;
        }
    }
    if (disable) (void) queue_command(stream, disable);
    for (;;) {
        const firebase_command_t *next = NULL;
        for (size_t i = 0; i < count; ++i) {
            const firebase_command_t *command = &stream->commands[i];
            if (command->type != FIREBASE_COMMAND_DISABLE && command->sequence > stream->last_sequence &&
                (!next || command->sequence < next->sequence)) {
                next = command;
            }
        }
        if (!next || !queue_command(stream, next)) break;
    }
    return true;
}

static bool open_stream(firebase_https_connection_t *connection, stream_context_t *stream)
{
    esp_http_client_handle_t client = connection->http;
    for (unsigned redirects = 0; redirects <= 2; ++redirects) {
        if (!stream_current(stream) || esp_http_client_open(client, 0) != ESP_OK ||
            esp_http_client_fetch_headers(client) < 0 ||
            esp_timer_get_time() >= connection->deadline_us) return false;
        const int status = esp_http_client_get_status_code(client);
        if (status == 200) return true;
        if (status != 307 || esp_http_client_set_redirection(client) != ESP_OK ||
            esp_http_client_get_url(client, stream->url, sizeof(stream->url)) != ESP_OK ||
            !firebase_database_redirect_valid(stream->url)) return false;
        (void) esp_http_client_close(client);
    }
    return false;
}

static bool listen_for_commands(stream_context_t *stream)
{
    (void) snprintf(stream->url, sizeof(stream->url), "%s/devices/%s/commands.json?auth=%s",
                    CONFIG_P1S_FIREBASE_DATABASE_URL, stream->uid, stream->token);
    firebase_https_connection_t connection;
    if (firebase_https_init(&connection, HTTP_METHOD_GET, stream->url, STREAM_STARTUP_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    const int64_t startup_deadline_us = connection.deadline_us;
    esp_http_client_handle_t http = connection.http;
    if (esp_http_client_set_header(http, "Accept", "text/event-stream") == ESP_OK &&
        open_stream(&connection, stream)) {
        char received[256];
        stream->last_activity_ms = monotonic_ms();
        while (stream_current(stream) && !activity_expired(stream->last_activity_ms, monotonic_ms())) {
            /* A connection is ready only after its first complete snapshot. */
            const int64_t now_us = esp_timer_get_time();
            if (!stream->ready && now_us >= startup_deadline_us) break;
            const int64_t read_deadline_us = now_us + STREAM_READ_TIMEOUT_MS * 1000LL;
            connection.deadline_us = !stream->ready && startup_deadline_us < read_deadline_us
                ? startup_deadline_us : read_deadline_us;
            const int count = esp_http_client_read(http, received, sizeof(received));
            if (count == -ESP_ERR_HTTP_EAGAIN) continue;
            if (count <= 0 || !firebase_sse_feed(&stream->parser, received, (size_t) count, handle_event, stream)) break;
        }
    }
    const bool was_ready = stream->ready;
    if (was_ready) close_channel(stream);
    firebase_https_cleanup(&connection);
    return was_ready;
}

static void stream_task(void *arg)
{
    stream_context_t *stream = arg;
    size_t failures = 0;
    for (;;) {
        const unsigned previous_generation = stream->generation;
        stream->parser.used = 0;
        stream->parser.line_start = 0;
        stream->last_sequence = 0;
        stream->seen_index = 0;
        memset(stream->seen, 0, sizeof(stream->seen));
        taskENTER_CRITICAL(&s_lock);
        stream->generation = s_generation;
        memcpy(stream->token, s_token, sizeof(stream->token));
        if (strcmp(stream->uid, s_uid) != 0) stream->next_session_number = 0;
        memcpy(stream->uid, s_uid, sizeof(stream->uid));
        taskEXIT_CRITICAL(&s_lock);
        if (previous_generation != stream->generation) failures = 0;
        if (stream->token[0]) {
            if (wifi_manager_connection_generation(&stream->wifi_generation)) {
                new_session(stream);
                if (listen_for_commands(stream)) failures = 0;
            }
            (void) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wifi_manager_backoff_ms(failures)));
            if (failures < 4U) ++failures;
        } else {
            (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

esp_err_t firebase_commands_start(void)
{
    if (s_stream_task) return ESP_OK;
    if (!s_enable_commit_lock) {
        s_enable_commit_lock = xSemaphoreCreateMutexStatic(&s_enable_commit_lock_storage);
        if (!s_enable_commit_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_commands) {
        s_commands = xQueueCreate(COMMAND_QUEUE_DEPTH, sizeof(queued_command_t));
        s_disable = xQueueCreate(1, sizeof(queued_command_t));
        s_results = xQueueCreate(RESULT_QUEUE_DEPTH, sizeof(firebase_command_result_t));
        if (!s_commands || !s_disable || !s_results) {
            if (s_commands) vQueueDelete(s_commands);
            if (s_disable) vQueueDelete(s_disable);
            if (s_results) vQueueDelete(s_results);
            s_commands = s_disable = s_results = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    /* Workers started before a failed allocation stay idle until a retry
     * connects the stream. No task can run with a partly initialized queue. */
    if (!s_command_task &&
        xTaskCreate(command_task, "firebase_dispatch", COMMAND_TASK_STACK, s_commands, 3, &s_command_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_disable_task &&
        xTaskCreate(command_task, "firebase_disable", DISABLE_TASK_STACK, s_disable, 4, &s_disable_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    stream_context_t *stream = calloc(1, sizeof(*stream));
    if (!stream) return ESP_ERR_NO_MEM;
    app_runtime_set_relay_start_guard(&s_enable_guard);
    if (xTaskCreate(stream_task, "firebase_commands", STREAM_TASK_STACK, stream, 3, &s_stream_task) != pdPASS) {
        free(stream);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif
