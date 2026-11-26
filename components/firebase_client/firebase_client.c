#include "firebase_client.h"

#include "sdkconfig.h"

#ifdef CONFIG_P1S_FIREBASE_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_runtime.h"
#include "cJSON.h"
#include "clock_service.h"
#include "device_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "firebase_commands.h"
#include "firebase_https.h"
#include "firebase_protocol.h"
#include "firmware_update.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printer_control.h"
#include "setup_portal.h"
#include "wifi_manager.h"

#define CLOCK_SYNC_INTERVAL_SECONDS (15U * 60U)
#define CLOCK_SYNC_RETRY_MS 30000U
/* lwIP may delay sending its first request after the SNTP client starts. */
#ifdef CONFIG_LWIP_SNTP_STARTUP_DELAY
#define CLOCK_SYNC_STARTUP_DELAY_MS CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY
#else
#define CLOCK_SYNC_STARTUP_DELAY_MS 0U
#endif
#define CLOCK_SYNC_TIMEOUT_MS (4500U + CLOCK_SYNC_STARTUP_DELAY_MS)
#define STATUS_INTERVAL_MS 15000U
#define RESULT_RETRY_MS 5000U
#define PAIR_POLL_MS 3000U
#define RECOVERY_REGISTRATION_RETRY_MS 30000U
#define ACCOUNT_DISABLED_RETRY_MS 60000U
#define TOKEN_REFRESH_MARGIN_MS 60000U
#define CLOUD_TASK_STACK 9216U
#define JSON_TYPE "application/json"
#define DATABASE_URL CONFIG_P1S_FIREBASE_DATABASE_URL
#define API_KEY CONFIG_P1S_FIREBASE_API_KEY
#define PAIRING_URL CONFIG_P1S_FIREBASE_PAIRING_URL

_Static_assert(DEVICE_FIREBASE_UID_BYTES == FIREBASE_UID_BYTES, "Firebase UID storage");
_Static_assert(DEVICE_FIREBASE_REFRESH_BYTES == FIREBASE_REFRESH_TOKEN_BYTES, "Firebase token storage");
_Static_assert(SETUP_PAIRING_CODE_BYTES == FIREBASE_PAIRING_CODE_BYTES, "Firebase setup code");

static const char *TAG = "firebase_client";
static portMUX_TYPE s_pairing_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_pairing_code[FIREBASE_PAIRING_CODE_BYTES];
static uint64_t s_pairing_expires;
static setup_pairing_state_t s_pairing_state;
static TaskHandle_t s_worker;

typedef struct {
    device_firebase_session_t session;
    firebase_pairing_session_t pairing;
    firebase_pairing_claim_t claim;
    firebase_auth_tokens_t tokens;
    char id_token[FIREBASE_ID_TOKEN_BYTES];
    char url[DEVICE_FIREBASE_DATABASE_BYTES + FIREBASE_UID_BYTES + FIREBASE_ID_TOKEN_BYTES + FIREBASE_COMMAND_ID_BYTES + 64];
    char body[FIREBASE_REFRESH_TOKEN_BYTES * 3 + 128];
    char response[4096];
    uint64_t token_deadline, refresh_at, pairing_deadline;
    uint64_t next_status_at, status_retry_at, result_retry_at, retry_at;
    uint64_t clock_retry_at, recovery_retry_at;
    size_t failures, status_failures;
    unsigned token_generation, result_token_generation;
    heat_fault_t uploaded_fault;
    firebase_command_status_t uploaded_channel;
    bool uploaded_restore_pending;
    bool storage_error, refresh_rejected, recovery_rejected, account_disabled;
    bool uploaded, result_requested_refresh;
    bool clock_synced;
} firebase_client_state_t;

static uint64_t now_ms(void)
{
    return (uint64_t) esp_timer_get_time() / 1000U;
}

static bool has_session(const firebase_client_state_t *client)
{
    return !client->refresh_rejected && client->session.refresh_token[0];
}

static bool pairing_needs_usb(const firebase_client_state_t *client)
{
    return client->storage_error || (client->refresh_rejected &&
        (!client->session.recovery_key[0] || client->recovery_rejected));
}

static void publish_pairing(const firebase_client_state_t *client)
{
    const bool authenticated = has_session(client);
    const setup_pairing_state_t state = pairing_needs_usb(client) ? SETUP_PAIRING_USB_REQUIRED :
        client->account_disabled ? SETUP_PAIRING_ACCOUNT_DISABLED : SETUP_PAIRING_CODE;
    taskENTER_CRITICAL(&s_pairing_lock);
    memcpy(s_pairing_code, client->pairing.code, sizeof(s_pairing_code));
    s_pairing_expires = authenticated || state != SETUP_PAIRING_CODE ? 0 : client->pairing_deadline;
    s_pairing_state = state;
    taskEXIT_CRITICAL(&s_pairing_lock);
    wifi_manager_set_paired(authenticated && !client->session.pairing_pending && state == SETUP_PAIRING_CODE);
}

static setup_pairing_state_t get_pairing_code(char code[SETUP_PAIRING_CODE_BYTES])
{
    const uint64_t now = now_ms();
    taskENTER_CRITICAL(&s_pairing_lock);
    if (now < s_pairing_expires) memcpy(code, s_pairing_code, sizeof(s_pairing_code));
    else code[0] = '\0';
    const setup_pairing_state_t state = s_pairing_state;
    taskEXIT_CRITICAL(&s_pairing_lock);
    return state;
}

static esp_err_t save_session(firebase_client_state_t *client, const char *uid, bool pairing_pending)
{
    const bool needs_key = !client->session.recovery_key[0] || strcmp(client->session.uid, uid) != 0;
    if (needs_key || client->session.pairing_pending != pairing_pending ||
        strcmp(client->session.refresh_token, client->tokens.refresh_token) != 0) {
        device_firebase_session_t next = client->session;
        next.pairing_pending = pairing_pending;
        if (needs_key) {
            static const char hex[] = "0123456789abcdef";
            uint8_t random[(sizeof(next.recovery_key) - 1U) / 2U];
            esp_fill_random(random, sizeof(random));
            for (size_t i = 0; i < sizeof(random); ++i) {
                next.recovery_key[2 * i] = hex[random[i] >> 4];
                next.recovery_key[2 * i + 1] = hex[random[i] & 15U];
            }
            next.recovery_key[sizeof(next.recovery_key) - 1U] = '\0';
            next.recovery_pending = true;
        }
        strcpy(next.database_url, DATABASE_URL);
        strcpy(next.uid, uid);
        strcpy(next.refresh_token, client->tokens.refresh_token);
        const esp_err_t err = device_config_save_firebase(&next);
        if (err != ESP_OK) return err;
        client->session = next;
    }
    memcpy(client->id_token, client->tokens.id_token, sizeof(client->id_token));
    ++client->token_generation;
    const uint64_t lifetime = (uint64_t) client->tokens.expires_in * 1000U;
    const uint64_t margin = lifetime > TOKEN_REFRESH_MARGIN_MS ? TOKEN_REFRESH_MARGIN_MS : lifetime / 2;
    client->token_deadline = now_ms() + lifetime;
    client->refresh_at = client->token_deadline - margin;
    client->refresh_rejected = false;
    client->account_disabled = false;
    return ESP_OK;
}

static bool has_provisional_enrollment(const firebase_client_state_t *client)
{
    return client->session.pairing_pending && client->session.recovery_pending;
}

static void reject_enrollment(firebase_client_state_t *client)
{
    client->account_disabled = false;
    client->refresh_rejected = true;
    if (has_provisional_enrollment(client)) {
        /* Start a new claim once the server has expired an unconfirmed identity. */
        memset(&client->session, 0, sizeof(client->session));
        client->refresh_rejected = false;
    }
    client->id_token[0] = '\0';
    client->uploaded = false;
}

static esp_err_t mark_account_disabled(firebase_client_state_t *client)
{
    /* Retain the stored enrollment so that re-enabling the account can restore it. */
    client->account_disabled = true;
    client->id_token[0] = '\0';
    client->refresh_at = 0;
    client->uploaded = false;
    firebase_commands_set_auth(client->session.uid, NULL);
    return ESP_ERR_NOT_ALLOWED;
}

static esp_err_t refresh_session(firebase_client_state_t *client)
{
    if (!firebase_encode_refresh_form(client->session.refresh_token, client->body, sizeof(client->body))) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    (void) snprintf(client->url, sizeof(client->url), "https://securetoken.googleapis.com/v1/token?key=%s", API_KEY);
    int status;
    esp_err_t err = firebase_https_request(HTTP_METHOD_POST, client->url,
        "application/x-www-form-urlencoded", client->body, client->response, sizeof(client->response), &status);
    if (err != ESP_OK) return err;
    if (status == 400) {
        const firebase_auth_error_t error = firebase_auth_error(client->response);
        if (error == FIREBASE_AUTH_ACCOUNT_DISABLED) return mark_account_disabled(client);
        if (error == FIREBASE_AUTH_REFRESH_REJECTED) {
            reject_enrollment(client);
            return ESP_ERR_NOT_ALLOWED;
        }
    }
    if (status != 200 || !firebase_parse_auth_tokens(client->response, true, client->session.uid, &client->tokens)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return save_session(client, client->session.uid, client->session.pairing_pending);
}

static esp_err_t pairing_post(firebase_client_state_t *client, const char *action, int *status)
{
    (void) snprintf(client->url, sizeof(client->url), "%s/%s", PAIRING_URL, action);
    return firebase_https_request(HTTP_METHOD_POST, client->url, JSON_TYPE, client->body,
                                  client->response, sizeof(client->response), status);
}

static esp_err_t begin_pairing(firebase_client_state_t *client)
{
    if (client->session.uid[0]) {
        (void) snprintf(client->body, sizeof(client->body), "{\"deviceUid\":\"%s\",\"recoveryKey\":\"%s\"}",
                        client->session.uid, client->session.recovery_key);
    } else {
        strcpy(client->body, "{\"deviceUid\":\"\"}");
    }
    int status;
    esp_err_t err = pairing_post(client, "begin", &status);
    if (err != ESP_OK) return err;
    if (firebase_pairing_account_disabled(status, client->response)) return mark_account_disabled(client);
    if (client->session.uid[0] && firebase_pairing_recovery_rejected(status, client->response)) {
        client->account_disabled = false;
        client->recovery_rejected = true;
        return ESP_ERR_NOT_ALLOWED;
    }
    if (status != 200 || !firebase_parse_pairing_session(client->response, &client->pairing)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    client->pairing_deadline = now_ms() + (uint64_t) client->pairing.expires_in * 1000U;
    client->account_disabled = false;
    return ESP_OK;
}

static esp_err_t poll_pairing(firebase_client_state_t *client)
{
    (void) snprintf(client->body, sizeof(client->body), "{\"sessionSecret\":\"%s\"}", client->pairing.secret);
    int status;
    esp_err_t err = pairing_post(client, "poll", &status);
    if (err != ESP_OK) return err;
    if (firebase_pairing_account_disabled(status, client->response)) return mark_account_disabled(client);
    if (status == 202 || status == 410) {
        if (status == 410) client->pairing_deadline = 0;
        return ESP_OK;
    }
    if (status != 200 || !firebase_parse_pairing_claim(client->response, &client->claim) ||
        (client->session.uid[0] && strcmp(client->session.uid, client->claim.uid) != 0)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    (void) snprintf(client->body, sizeof(client->body), "{\"token\":\"%s\",\"returnSecureToken\":true}",
                    client->claim.custom_token);
    (void) snprintf(client->url, sizeof(client->url),
                    "https://identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken?key=%s", API_KEY);
    err = firebase_https_request(HTTP_METHOD_POST, client->url, JSON_TYPE, client->body,
                                 client->response, sizeof(client->response), &status);
    if (err != ESP_OK) return err;
    if (status == 400 && firebase_auth_error(client->response) == FIREBASE_AUTH_ACCOUNT_DISABLED) {
        return mark_account_disabled(client);
    }
    if (status != 200 || !firebase_parse_auth_tokens(client->response, false, client->claim.uid, &client->tokens)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = save_session(client, client->claim.uid, true);
    if (err == ESP_OK) {
        client->pairing_deadline = 0;
        memset(&client->pairing, 0, sizeof(client->pairing));
    }
    return err;
}

static esp_err_t finish_pairing(firebase_client_state_t *client)
{
    (void) snprintf(client->body, sizeof(client->body), "{\"idToken\":\"%s\",\"recoveryKey\":\"%s\"}",
                    client->id_token, client->session.recovery_key);
    int status;
    const esp_err_t err = pairing_post(client, "complete", &status);
    if (err != ESP_OK) return err;
    if (firebase_pairing_account_disabled(status, client->response)) return mark_account_disabled(client);
    if (status == 401) client->refresh_at = 0;
    if (has_provisional_enrollment(client) && firebase_pairing_recovery_rejected(status, client->response)) {
        reject_enrollment(client);
    }
    if (status != 204) return ESP_ERR_INVALID_RESPONSE;

    const bool pairing_pending = client->session.pairing_pending;
    const bool recovery_pending = client->session.recovery_pending;
    client->session.pairing_pending = false;
    client->session.recovery_pending = false;
    const esp_err_t saved = device_config_save_firebase(&client->session);
    if (saved != ESP_OK) {
        client->session.pairing_pending = pairing_pending;
        client->session.recovery_pending = recovery_pending;
        ESP_LOGW(TAG, "could not save pairing confirmation: %s", esp_err_to_name(saved));
    }
    return saved;
}

static bool status_json(const app_status_t *heat, const wifi_manager_status_t *wifi,
                        const firebase_command_status_t *channel, char *body, size_t capacity)
{
    printer_control_status_t printer;
    printer_control_get_status(&printer);
    firmware_status_t firmware;
    firmware_update_get_status(&firmware);
    const uint32_t heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t free_heap = heap_caps_get_free_size(heap_caps);
    const size_t minimum_free_heap = heap_caps_get_minimum_free_size(heap_caps);

    cJSON *root = cJSON_CreateObject();
    cJSON *updated = root ? cJSON_AddObjectToObject(root, "updatedAt") : NULL;
    const bool status_ready = updated && cJSON_AddStringToObject(updated, ".sv", "timestamp") &&
        cJSON_AddNumberToObject(root, "schemaVersion", 1) &&
        cJSON_AddStringToObject(root, "name", wifi->controller_name) &&
        (!wifi->board_id[0] || cJSON_AddStringToObject(root, "boardId", wifi->board_id)) &&
        cJSON_AddStringToObject(root, "firmwareVersion", custom_app_desc.version) &&
        cJSON_AddStringToObject(root, "state", firebase_heat_state_name(heat->state)) &&
        cJSON_AddStringToObject(root, "fault", firebase_heat_fault_name(heat->fault)) &&
        cJSON_AddNumberToObject(root, "targetC", heat->target_c) &&
        cJSON_AddNumberToObject(root, "restartC", heat->restart_c) &&
        cJSON_AddNumberToObject(root, "calibrationOffsetC", heat->calibration_offset_c) &&
        cJSON_AddBoolToObject(root, "temperatureValid", heat->temperature.valid) &&
        (!heat->temperature.valid || cJSON_AddNumberToObject(root, "temperatureC", heat->temperature.filtered_c)) &&
        cJSON_AddBoolToObject(root, "heatEnabled", heat->enabled) &&
        cJSON_AddBoolToObject(root, "restorePending", heat->restore_pending) &&
        cJSON_AddBoolToObject(root, "heaterOn", heat->heater_on) &&
        cJSON_AddBoolToObject(root, "fanOn", heat->fan_on) &&
        cJSON_AddBoolToObject(root, "printerConfigured", printer.configured) &&
        cJSON_AddBoolToObject(root, "canMonitor", printer.can_monitor) &&
        cJSON_AddBoolToObject(root, "printerConnected", printer.link.connected) &&
        cJSON_AddBoolToObject(root, "printerStateKnown", printer.link.state_known) &&
        cJSON_AddNumberToObject(root, "printerState", printer.link.state) &&
        cJSON_AddNumberToObject(root, "monitorPhase", printer.monitor.phase) &&
#ifdef CONFIG_P1S_FIREBASE_PUBLIC_RELEASE
        cJSON_AddBoolToObject(root, "canUpdate", true) &&
#endif
        cJSON_AddNumberToObject(root, "updatePhase", firmware.phase) &&
        cJSON_AddNumberToObject(root, "updateResult", firmware.result) &&
        cJSON_AddNumberToObject(root, "updateReceived", firmware.received) &&
        cJSON_AddNumberToObject(root, "updateTotal", firmware.total) &&
        cJSON_AddBoolToObject(root, "commandReady", channel->ready) &&
        cJSON_AddStringToObject(root, "sessionId", channel->session_id);
    if (status_ready) {
        /* Heap reporting is optional if there is too little memory to add it. */
        cJSON *heap = cJSON_AddObjectToObject(root, "heap");
        if (heap && (!cJSON_AddNumberToObject(heap, "freeBytes", free_heap) ||
                     !cJSON_AddNumberToObject(heap, "minimumFreeBytes", minimum_free_heap))) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "heap");
        }
    }
    const bool written = status_ready && cJSON_PrintPreallocated(root, body, (int) capacity, false);
    cJSON_Delete(root);
    return written;
}

static esp_err_t upload_status(firebase_client_state_t *client, const app_status_t *heat,
                               const wifi_manager_status_t *wifi, const firebase_command_status_t *channel)
{
    if (!status_json(heat, wifi, channel, client->body, sizeof(client->body))) return ESP_ERR_NO_MEM;
    (void) snprintf(client->url, sizeof(client->url), "%s/devices/%s/latest.json?auth=%s&print=silent",
                    DATABASE_URL, client->session.uid, client->id_token);
    int status;
    const esp_err_t err = firebase_https_request(HTTP_METHOD_PUT, client->url, JSON_TYPE, client->body,
                                                NULL, 0, &status);
    if (err != ESP_OK) return err;
    if (status == 401) client->refresh_at = 0;
    if (status != 204) return ESP_ERR_INVALID_RESPONSE;
    client->next_status_at = now_ms() + STATUS_INTERVAL_MS;
    client->uploaded_fault = heat->fault;
    client->uploaded_restore_pending = heat->restore_pending;
    client->uploaded_channel = *channel;
    client->uploaded = true;
    return ESP_OK;
}

static void upload_status_if_due(firebase_client_state_t *client, const wifi_manager_status_t *wifi)
{
    const uint64_t now = now_ms();
    if (now < client->status_retry_at) return;

    firebase_command_status_t channel;
    firebase_commands_get_status(&channel);
    app_status_t heat;
    app_runtime_get_status(&heat);
    if (client->uploaded && now < client->next_status_at && client->uploaded_fault == heat.fault &&
        client->uploaded_restore_pending == heat.restore_pending &&
        channel.ready == client->uploaded_channel.ready &&
        strcmp(channel.session_id, client->uploaded_channel.session_id) == 0) {
        return;
    }
    if (upload_status(client, &heat, wifi, &channel) == ESP_OK) {
        client->status_retry_at = 0;
        client->status_failures = 0;
        return;
    }
    client->uploaded = false;
    client->status_retry_at = now_ms() + wifi_manager_backoff_ms(client->status_failures);
    if (client->status_failures < 4U) ++client->status_failures;
}

static esp_err_t upload_result(firebase_client_state_t *client)
{
    firebase_command_result_t result;
    if (!firebase_commands_peek_result(&result)) return ESP_OK;
    if (strcmp(result.uid, client->session.uid) != 0 || now_ms() >= result.deliver_until_ms) {
        firebase_commands_remove_result();
        client->result_requested_refresh = false;
        return ESP_OK;
    }
    if (client->result_requested_refresh && client->token_generation == client->result_token_generation) {
        return ESP_ERR_INVALID_STATE;
    }
    (void) snprintf(client->body, sizeof(client->body),
                    "{\"id\":\"%s\",\"status\":\"%s\",\"reason\":\"%s\",\"timestamp\":{\".sv\":\"timestamp\"}}",
                    result.id, result.status, result.reason ? result.reason : "");
    (void) snprintf(client->url, sizeof(client->url), "%s/devices/%s/results/%s.json?auth=%s&print=silent",
                    DATABASE_URL, client->session.uid, result.id, client->id_token);
    int status;
    const esp_err_t err = firebase_https_request(HTTP_METHOD_PUT, client->url, JSON_TYPE, client->body,
                                                NULL, 0, &status);
    if (err != ESP_OK) return err;
    const firebase_result_upload_action_t action = firebase_result_upload_action(status, client->result_requested_refresh);
    if (action == FIREBASE_RESULT_REFRESH_AUTH) {
        client->result_token_generation = client->token_generation;
        client->result_requested_refresh = true;
        client->refresh_at = 0;
    }
    if (action == FIREBASE_RESULT_REFRESH_AUTH || action == FIREBASE_RESULT_RETRY) return ESP_ERR_INVALID_RESPONSE;
    firebase_commands_remove_result();
    client->result_requested_refresh = false;
    if (action == FIREBASE_RESULT_UPLOADED) client->next_status_at = 0;
    return ESP_OK;
}

static void load_session(firebase_client_state_t *client)
{
    const esp_err_t stored = device_config_load_firebase(&client->session);
    if (stored == ESP_OK) {
        client->storage_error = !firebase_database_url_valid(client->session.database_url) ||
            !firebase_uid_valid(client->session.uid) ||
            !firebase_encode_refresh_form(client->session.refresh_token, client->body, sizeof(client->body));
    } else {
        client->storage_error = stored != ESP_ERR_NOT_FOUND;
    }
    if (!client->storage_error && strcmp(client->session.database_url, DATABASE_URL) != 0) {
        memset(&client->session, 0, sizeof(client->session));
    }
}

static esp_err_t start_portal(void)
{
    return setup_portal_start(get_pairing_code);
}

static esp_err_t cloud_tick(firebase_client_state_t *client, const wifi_manager_status_t *wifi, uint64_t now)
{
    if (client->storage_error) return ESP_ERR_INVALID_STATE;
    if (!clock_service_is_recent(CLOCK_SYNC_INTERVAL_SECONDS) && now >= client->clock_retry_at) {
        if (clock_service_sync("time.google.com", CLOCK_SYNC_TIMEOUT_MS) == ESP_OK) {
            client->clock_synced = true;
        }
        now = now_ms();
        client->clock_retry_at = now + CLOCK_SYNC_RETRY_MS;
    }
    /* TLS needs an initialized clock. The command stream enforces its own
     * maximum sync age so telemetry can continue during an SNTP outage. */
    if (!client->clock_synced) {
        client->clock_synced = clock_service_is_recent(CLOCK_SYNC_INTERVAL_SECONDS);
        if (!client->clock_synced) return ESP_ERR_TIMEOUT;
    }
    if (!has_session(client)) {
        firebase_commands_set_auth(client->session.uid, NULL);
        if (!wifi->ap_active || pairing_needs_usb(client)) return ESP_OK;
        if (now >= client->pairing_deadline) {
            memset(&client->pairing, 0, sizeof(client->pairing));
            return begin_pairing(client);
        }
        return poll_pairing(client);
    }
    if (now >= client->refresh_at) {
        const esp_err_t err = refresh_session(client);
        if (err != ESP_OK) {
            if (client->account_disabled) return err;
            if (!has_session(client) || now_ms() >= client->token_deadline) {
                firebase_commands_set_auth(client->session.uid, NULL);
                client->uploaded = false;
                return err;
            }
            client->refresh_at = now_ms() + 5000U;
            if (client->refresh_at > client->token_deadline) client->refresh_at = client->token_deadline;
        }
    }
    if (client->session.pairing_pending) {
        const esp_err_t err = finish_pairing(client);
        if (err != ESP_OK) return err;
    }
    firebase_commands_set_auth(client->session.uid, client->id_token);
    if (now_ms() >= client->result_retry_at && upload_result(client) != ESP_OK) {
        client->result_retry_at = now_ms() + RESULT_RETRY_MS;
    }
    upload_status_if_due(client, wifi);
    /* A legacy enrollment continues serving commands while it registers its key. */
    if (client->session.recovery_pending && now_ms() >= client->recovery_retry_at &&
        finish_pairing(client) != ESP_OK) {
        client->recovery_retry_at = now_ms() + RECOVERY_REGISTRATION_RETRY_MS;
    }
    return ESP_OK;
}

static void cloud_task(void *arg)
{
    firebase_client_state_t *client = arg;
    load_session(client);
    firmware_trial_note_cloud_storage_ready(!client->storage_error);
    publish_pairing(client);
    wifi_manager_set_setup_portal(start_portal, setup_portal_stop);
    for (;;) {
        wifi_manager_status_t wifi;
        wifi_manager_get_status(&wifi);
        uint64_t now = now_ms();
        if (!wifi.connected || wifi.applying) {
            firebase_commands_set_auth(client->session.uid, NULL);
            client->uploaded = false;
            client->status_retry_at = 0;
            client->result_retry_at = 0;
            client->retry_at = 0;
            client->clock_retry_at = 0;
            client->failures = 0;
            client->status_failures = 0;
        } else if (now >= client->retry_at) {
            const esp_err_t err = cloud_tick(client, &wifi, now);
            now = now_ms();
            if (client->account_disabled) {
                client->retry_at = now + ACCOUNT_DISABLED_RETRY_MS;
            } else if (err == ESP_OK) {
                client->failures = 0;
                client->retry_at = has_session(client) ? now : now + PAIR_POLL_MS;
            } else {
                client->retry_at = now + wifi_manager_backoff_ms(client->failures);
                if (client->failures < 4U) ++client->failures;
            }
            publish_pairing(client);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t firebase_client_start(void)
{
    if (s_worker) return ESP_OK;
    if (!firebase_database_url_valid(DATABASE_URL) || !firebase_uid_valid(API_KEY) ||
        !firebase_pairing_url_valid(PAIRING_URL)) {
        ESP_LOGE(TAG, "Invalid Firebase project configuration");
        return ESP_ERR_INVALID_ARG;
    }
    firebase_client_state_t *state = calloc(1, sizeof(*state));
    if (!state) return ESP_ERR_NO_MEM;
    const esp_err_t commands = firebase_commands_start();
    if (commands != ESP_OK) {
        free(state);
        return commands;
    }
    if (xTaskCreate(cloud_task, "firebase", CLOUD_TASK_STACK, state, 3, &s_worker) != pdPASS) {
        free(state);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else

esp_err_t firebase_client_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
