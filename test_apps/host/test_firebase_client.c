#include <assert.h>

/* Exercise the cloud worker with deterministic time, storage and HTTPS. */
#include "../../components/firebase_client/firebase_client.c"

static struct {
    uint64_t now;
    uint32_t clock_reply_ms;
    esp_err_t stored;
    bool paired, command_auth, result_pending, clock_synced;
    int status_response, result_response, refresh_response, pairing_response;
    const char *refresh_body, *pairing_body;
    unsigned status_writes, result_writes, pairing_requests;
    heat_fault_t fault;
    setup_pairing_code_fn read_code;
} s;

static const device_firebase_session_t saved = {
    .database_url = DATABASE_URL, .uid = "test-device", .refresh_token = "test-refresh",
    .recovery_key = "0000000000000000000000000000000000000000000000000000000000000000",
};
static const firebase_command_result_t disable_result = {
    .uid = "test-device", .id = "disable", .status = "applied", .deliver_until_ms = UINT64_MAX,
};
static const wifi_manager_status_t connected = {
    .connected = true, .ap_active = true, .board_id = "7C87CE01020A",
};
const firmware_identity_t custom_app_desc = {.version = "1.0.1"};

int64_t esp_timer_get_time(void) { return (int64_t) s.now * 1000; }
size_t heap_caps_get_free_size(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return 80092;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return 25328;
}

void esp_fill_random(void *buffer, size_t length) { memset(buffer, 0, length); }
bool clock_service_is_recent(uint32_t age)
{ (void) age; return !s.clock_reply_ms || s.clock_synced; }
esp_err_t clock_service_sync(const char *host, uint32_t timeout)
{
    (void) host;
    if (s.clock_reply_ms >= timeout) return ESP_ERR_TIMEOUT;
    s.now += s.clock_reply_ms;
    s.clock_synced = true;
    return ESP_OK;
}

esp_err_t device_config_load_firebase(device_firebase_session_t *out)
{
    if (s.stored == ESP_OK) *out = saved;
    return s.stored;
}
esp_err_t device_config_save_firebase(const device_firebase_session_t *session)
{ (void) session; return ESP_OK; }
void firmware_trial_note_cloud_storage_ready(bool ready) { (void) ready; }
void wifi_manager_set_paired(bool paired) { s.paired = paired; }
void wifi_manager_get_status(wifi_manager_status_t *out) { *out = connected; }
void wifi_manager_set_setup_portal(wifi_setup_portal_fn start, wifi_setup_portal_fn stop)
{ (void) start; (void) stop; }
esp_err_t setup_portal_start(setup_pairing_code_fn get_code)
{ s.read_code = get_code; return ESP_OK; }
esp_err_t setup_portal_stop(void) { return ESP_OK; }

void firebase_commands_set_auth(const char *uid, const char *token)
{ (void) uid; s.command_auth = token && token[0]; }
void firebase_commands_get_status(firebase_command_status_t *out)
{ *out = (firebase_command_status_t) {.ready = true, .session_id = "0123456789abcdef0123456789abcdef"}; }
bool firebase_commands_peek_result(firebase_command_result_t *out)
{
    if (s.result_pending) *out = disable_result;
    return s.result_pending;
}
void firebase_commands_remove_result(void) { s.result_pending = false; }
esp_err_t firebase_commands_start(void) { return ESP_OK; }
void app_runtime_get_status(app_status_t *out) { *out = app_status_initial(); out->fault = s.fault; }
void printer_control_get_status(printer_control_status_t *out)
{ *out = (printer_control_status_t) {0}; }
void firmware_update_get_status(firmware_status_t *out) { *out = (firmware_status_t) {0}; }

BaseType_t xTaskCreate(void (*worker)(void *), const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *out)
{
    (void) worker;
    (void) name;
    (void) stack;
    (void) arg;
    (void) priority;
    (void) out;
    assert(false);
    return pdFALSE;
}
void vTaskDelay(TickType_t ticks) { (void) ticks; assert(false); }

esp_err_t firebase_https_request(esp_http_client_method_t method, const char *url,
                                 const char *type, const char *body, char *response,
                                 size_t capacity, int *status)
{
    if (method == HTTP_METHOD_POST) {
        const char *reply;
        if (strstr(url, "https://securetoken.googleapis.com/")) {
            assert(strcmp(type, "application/x-www-form-urlencoded") == 0);
            *status = s.refresh_response;
            reply = s.refresh_body;
        } else {
            assert(strcmp(url, PAIRING_URL "/begin") == 0 && strcmp(type, JSON_TYPE) == 0);
            ++s.pairing_requests;
            *status = s.pairing_response;
            reply = s.pairing_body;
        }
        assert(reply && response && strlen(reply) < capacity);
        strcpy(response, reply);
        return ESP_OK;
    }
    assert(method == HTTP_METHOD_PUT && strcmp(type, JSON_TYPE) == 0);
    if (strstr(url, "/latest.json?")) {
        assert(strstr(body, "\"boardId\":\"7C87CE01020A\""));
        assert(strstr(body, "\"heap\":{\"freeBytes\":80092,\"minimumFreeBytes\":25328}"));
        ++s.status_writes;
        *status = s.status_response;
    } else {
        assert(strstr(url, "/results/disable.json?"));
        assert(strstr(body, "\"status\":\"applied\""));
        ++s.result_writes;
        *status = s.result_response;
    }
    return ESP_OK;
}

static void reset_fixture(void)
{
    memset(&s, 0, sizeof(s));
    s.now = 1000;
    s.status_response = s.result_response = 204;
}

static void start_authenticated(firebase_client_state_t *client)
{
    load_session(client);
    assert(!client->storage_error);
    client->clock_synced = true;
    client->refresh_at = UINT64_MAX;
    strcpy(client->id_token, "test-token");
}

static void delayed_clock_reply_allows_first_cloud_connection(void)
{
    reset_fixture();
    firebase_client_state_t client = {0};
    load_session(&client);
    s.clock_reply_ms = CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY + 250U;
    s.refresh_response = 200;
    s.refresh_body = "{\"id_token\":\"new-token\",\"refresh_token\":\"test-refresh\","
        "\"expires_in\":\"3600\",\"user_id\":\"test-device\"}";

    assert(cloud_tick(&client, &connected, s.now) == ESP_OK);
    assert(client.clock_synced && s.command_auth && s.status_writes == 1);
}

static void telemetry_failure_leaves_results_available(void)
{
    reset_fixture();
    firebase_client_state_t client = {0};
    start_authenticated(&client);
    s.status_response = 503;
    s.result_pending = true;
    assert(cloud_tick(&client, &connected, s.now) == ESP_OK);
    assert(s.status_writes == 1 && s.result_writes == 1 && !s.result_pending);

    /* A new result can leave during telemetry's retry delay. */
    s.now += 250;
    s.result_pending = true;
    assert(cloud_tick(&client, &connected, s.now) == ESP_OK);
    assert(s.status_writes == 1 && s.result_writes == 2 && !s.result_pending);

    s.now = 1000 + wifi_manager_backoff_ms(0);
    s.status_response = 204;
    assert(cloud_tick(&client, &connected, s.now) == ESP_OK);
    assert(s.status_writes == 2 && client.uploaded);
}

static void result_failure_leaves_telemetry_available(void)
{
    reset_fixture();
    firebase_client_state_t client = {0};
    start_authenticated(&client);
    s.result_response = 503;
    s.result_pending = true;
    assert(cloud_tick(&client, &connected, s.now) == ESP_OK);
    assert(s.status_writes == 1 && s.result_writes == 1 && s.result_pending);

    s.now += 250;
    s.fault = HEAT_FAULT_SENSOR_STALE;
    assert(cloud_tick(&client, &connected, s.now) == ESP_OK);
    assert(s.status_writes == 2 && s.result_writes == 1 && s.result_pending);

    /* A delivered result prompts fresh telemetry before the next heartbeat. */
    s.now = 1000 + RESULT_RETRY_MS;
    s.result_response = 204;
    assert(cloud_tick(&client, &connected, s.now) == ESP_OK);
    assert(s.status_writes == 3 && s.result_writes == 2 && !s.result_pending);
}

static void unreadable_enrollment_requires_usb_recovery(void)
{
    reset_fixture();
    firebase_client_state_t client = {0};
    s.stored = ESP_ERR_INVALID_RESPONSE;
    load_session(&client);
    publish_pairing(&client);
    assert(start_portal() == ESP_OK && s.read_code);
    char code[SETUP_PAIRING_CODE_BYTES] = "old-code";
    assert(!s.paired && s.read_code(code) == SETUP_PAIRING_USB_REQUIRED && code[0] == '\0');

    /* An empty partition still permits ordinary first-time pairing. */
    client = (firebase_client_state_t) {0};
    s.stored = ESP_ERR_NOT_FOUND;
    load_session(&client);
    publish_pairing(&client);
    assert(!s.paired && s.read_code(code) == SETUP_PAIRING_CODE && code[0] == '\0');
}

static void account_disable_preserves_enrollment_for_reactivation(void)
{
    reset_fixture();
    firebase_client_state_t client = {0};
    start_authenticated(&client);
    firebase_commands_set_auth(saved.uid, client.id_token);
    client.refresh_at = s.now;
    client.token_deadline = s.now + 60000;
    s.refresh_response = 400;
    s.refresh_body = "{\"error\":{\"message\":\"USER_DISABLED\"}}";
    assert(cloud_tick(&client, &connected, s.now) == ESP_ERR_NOT_ALLOWED);
    publish_pairing(&client);
    char code[SETUP_PAIRING_CODE_BYTES] = "old-code";
    assert(!s.paired && !s.command_auth && !s.status_writes && !s.pairing_requests);
    assert(get_pairing_code(code) == SETUP_PAIRING_ACCOUNT_DISABLED && code[0] == '\0');
    assert(has_session(&client) && strcmp(client.session.refresh_token, saved.refresh_token) == 0);

    s.now += ACCOUNT_DISABLED_RETRY_MS;
    s.refresh_response = 200;
    s.refresh_body = "{\"id_token\":\"new-token\",\"refresh_token\":\"test-refresh\","
        "\"expires_in\":\"3600\",\"user_id\":\"test-device\"}";
    assert(cloud_tick(&client, &connected, s.now) == ESP_OK);
    publish_pairing(&client);
    assert(s.paired && s.command_auth && s.status_writes == 1 && !s.pairing_requests);
}

static void disabled_recovery_hides_codes_until_the_account_is_available(void)
{
    reset_fixture();
    firebase_client_state_t client = {
        .session = saved, .clock_synced = true, .refresh_rejected = true,
    };
    s.pairing_response = 409;
    s.pairing_body = "{\"error\":\"device_disabled\"}";
    assert(cloud_tick(&client, &connected, s.now) == ESP_ERR_NOT_ALLOWED);
    publish_pairing(&client);
    char code[SETUP_PAIRING_CODE_BYTES];
    assert(!s.paired && s.pairing_requests == 1);
    assert(get_pairing_code(code) == SETUP_PAIRING_ACCOUNT_DISABLED && code[0] == '\0');

    s.now += ACCOUNT_DISABLED_RETRY_MS;
    s.pairing_response = 200;
    s.pairing_body = "{\"code\":\"TEST2345\",\"sessionSecret\":\"01234567890123456789012345678901\",\"expiresIn\":300}";
    assert(cloud_tick(&client, &connected, s.now) == ESP_OK);
    publish_pairing(&client);
    assert(!s.paired && s.pairing_requests == 2);
    assert(get_pairing_code(code) == SETUP_PAIRING_CODE && strcmp(code, "TEST2345") == 0);
}

static void expired_provisional_enrollment_can_pair_again(void)
{
    reset_fixture();
    firebase_client_state_t client = {.session = saved};
    client.session.pairing_pending = true;
    client.session.recovery_pending = true;
    reject_enrollment(&client);
    assert(!has_session(&client) && !pairing_needs_usb(&client));
    assert(!client.session.uid[0]);
}

int main(void)
{
    delayed_clock_reply_allows_first_cloud_connection();
    telemetry_failure_leaves_results_available();
    result_failure_leaves_telemetry_available();
    unreadable_enrollment_requires_usb_recovery();
    account_disable_preserves_enrollment_for_reactivation();
    disabled_recovery_hides_codes_until_the_account_is_available();
    expired_provisional_enrollment_can_pair_again();
    return 0;
}
