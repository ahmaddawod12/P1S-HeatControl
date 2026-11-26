#include "firebase_protocol.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "cJSON.h"

const char *firebase_heat_state_name(heat_state_t state)
{
    switch (state) {
    case HEAT_STATE_BOOT:
        return "boot";
    case HEAT_STATE_IDLE:
        return "idle";
    case HEAT_STATE_HEATING:
        return "heating";
    case HEAT_STATE_HOLDING:
        return "holding";
    case HEAT_STATE_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

const char *firebase_heat_fault_name(heat_fault_t fault)
{
    switch (fault) {
    case HEAT_FAULT_NONE:
        return "none";
    case HEAT_FAULT_SENSOR_OPEN:
        return "sensor_open";
    case HEAT_FAULT_SENSOR_SHORT:
        return "sensor_short";
    case HEAT_FAULT_SENSOR_RANGE:
        return "sensor_range";
    case HEAT_FAULT_SENSOR_INVALID:
        return "sensor_invalid";
    case HEAT_FAULT_SENSOR_STALE:
        return "sensor_stale";
    case HEAT_FAULT_OVERTEMP:
        return "overtemperature";
    case HEAT_FAULT_NVS:
        return "nvs";
    case HEAT_FAULT_OUTPUT_INIT:
        return "output_init";
    case HEAT_FAULT_RESTART_LOCKOUT:
        return "restart_lockout";
    default:
        return "unknown";
    }
}

static bool ascii_alnum(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool path_char(char c)
{
    return ascii_alnum(c) || c == '_' || c == '-';
}

static bool token_char(char c)
{
    return path_char(c) || c == '.';
}

static bool visible_ascii(char c)
{
    return c >= '!' && c <= '~';
}

static bool text_valid(const char *text, size_t capacity, bool (*valid_char)(char))
{
    if (!text || !text[0]) return false;
    size_t i = 0;
    for (; i < capacity && text[i] != '\0'; ++i) {
        if (!valid_char(text[i])) return false;
    }
    return i < capacity;
}

bool firebase_uid_valid(const char *uid)
{
    return text_valid(uid, FIREBASE_UID_BYTES, path_char);
}

static size_t https_host_length(const char *url)
{
    static const char prefix[] = "https://";
    if (!url || strncmp(url, prefix, sizeof(prefix) - 1) != 0) return 0;
    const char *host = url + sizeof(prefix) - 1;
    const size_t length = strcspn(host, "/:");
    if (length == 0 || length > 253 || !ascii_alnum(host[0]) || !ascii_alnum(host[length - 1])) {
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!ascii_alnum(host[i]) && host[i] != '.' && host[i] != '-') return 0;
    }
    return length;
}

static bool host_has_suffix(const char *host, size_t length, const char *suffix)
{
    const size_t suffix_length = strlen(suffix);
    return length > suffix_length && memcmp(host + length - suffix_length, suffix, suffix_length) == 0;
}

bool firebase_database_redirect_valid(const char *url)
{
    const size_t length = https_host_length(url);
    if (!length) return false;
    const char *host = url + strlen("https://");
    const char *path = host + length;
    /* ESP-IDF includes the default HTTPS port in esp_http_client_get_url(). */
    if (strncmp(path, ":443", 4) == 0) path += 4;
    if (*path && *path != '/') return false;
    return host_has_suffix(host, length, ".firebaseio.com") ||
        host_has_suffix(host, length, ".firebasedatabase.app");
}

bool firebase_database_url_valid(const char *url)
{
    return firebase_database_redirect_valid(url) && url[strlen("https://") + https_host_length(url)] == '\0';
}

bool firebase_pairing_url_valid(const char *url)
{
    const size_t length = https_host_length(url);
    if (!length || strlen(url) >= 244 || url[strlen(url) - 1] == '/') return false;
    for (const char *p = url + strlen("https://") + length; *p; ++p) {
        if (!path_char(*p) && *p != '/' && *p != '.') return false;
    }
    return true;
}

static cJSON *parse_object(const char *body)
{
    if (!body || strstr(body, "\\u0000")) return NULL;
    /* Bound recursion before passing a network response to cJSON. */
    unsigned depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char *p = body; *p; ++p) {
        if (escaped) {
            escaped = false;
        } else if (quoted && *p == '\\') {
            escaped = true;
        } else if (*p == '"') {
            quoted = !quoted;
        } else if (!quoted && (*p == '{' || *p == '[')) {
            if (++depth > 8) return NULL;
        } else if (!quoted && (*p == '}' || *p == ']')) {
            if (depth == 0) return NULL;
            --depth;
        }
    }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    if (cJSON_IsObject(root)) return root;
    cJSON_Delete(root);
    return NULL;
}

static const cJSON *field(const cJSON *root, const char *key)
{
    const cJSON *match = NULL;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, root) {
        if (child->string && strcmp(child->string, key) == 0) {
            if (match) return NULL;
            match = child;
        }
    }
    return match;
}

static const char *text_field(const cJSON *root, const char *key)
{
    return cJSON_GetStringValue(field(root, key));
}

static bool parse_token_lifetime(const char *text, uint32_t *out)
{
    if (!text || !text[0]) return false;
    uint32_t seconds = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || seconds > 8640) return false;
        seconds = seconds * 10 + (uint32_t) (*text - '0');
    }
    if (seconds == 0 || seconds > 86400) return false;
    *out = seconds;
    return true;
}

bool firebase_parse_pairing_session(const char *body, firebase_pairing_session_t *out)
{
    if (!out) return false;
    cJSON *root = parse_object(body);
    const char *code = text_field(root, "code");
    const char *secret = text_field(root, "sessionSecret");
    const cJSON *expires = field(root, "expiresIn");
    const double seconds = cJSON_GetNumberValue(expires);
    const bool valid = text_valid(code, sizeof(out->code), path_char) &&
        text_valid(secret, sizeof(out->secret), path_char) && strlen(secret) >= 32 &&
        seconds >= 1 && seconds <= 900 && floor(seconds) == seconds;
    if (valid) {
        memset(out, 0, sizeof(*out));
        strcpy(out->code, code);
        strcpy(out->secret, secret);
        out->expires_in = (uint32_t) seconds;
    }
    cJSON_Delete(root);
    return valid;
}

bool firebase_parse_pairing_claim(const char *body, firebase_pairing_claim_t *out)
{
    if (!out) return false;
    cJSON *root = parse_object(body);
    const char *uid = text_field(root, "deviceUid");
    const char *token = text_field(root, "customToken");
    const bool valid = firebase_uid_valid(uid) && text_valid(token, sizeof(out->custom_token), token_char);
    if (valid) {
        memset(out, 0, sizeof(*out));
        strcpy(out->uid, uid);
        strcpy(out->custom_token, token);
    }
    cJSON_Delete(root);
    return valid;
}

static bool pairing_error_matches(const char *body, const char *expected)
{
    cJSON *root = parse_object(body);
    const char *error = text_field(root, "error");
    const bool matches = error && strcmp(error, expected) == 0;
    cJSON_Delete(root);
    return matches;
}

bool firebase_pairing_recovery_rejected(int status, const char *body)
{
    return status == 403 && pairing_error_matches(body, "permission-denied");
}

bool firebase_pairing_account_disabled(int status, const char *body)
{
    return status == 409 && pairing_error_matches(body, "device_disabled");
}

bool firebase_parse_auth_tokens(const char *body, bool refresh, const char *expected_uid,
                               firebase_auth_tokens_t *out)
{
    if (!out || !firebase_uid_valid(expected_uid)) return false;
    cJSON *root = parse_object(body);
    const char *id = text_field(root, refresh ? "id_token" : "idToken");
    const char *token = text_field(root, refresh ? "refresh_token" : "refreshToken");
    const char *uid = text_field(root, "user_id");
    uint32_t lifetime;
    const bool valid = text_valid(id, sizeof(out->id_token), token_char) &&
        text_valid(token, sizeof(out->refresh_token), visible_ascii) &&
        (!refresh || (uid && strcmp(uid, expected_uid) == 0)) &&
        parse_token_lifetime(text_field(root, refresh ? "expires_in" : "expiresIn"), &lifetime);
    if (valid) {
        memset(out, 0, sizeof(*out));
        strcpy(out->id_token, id);
        strcpy(out->refresh_token, token);
        out->expires_in = lifetime;
    }
    cJSON_Delete(root);
    return valid;
}

firebase_auth_error_t firebase_auth_error(const char *body)
{
    cJSON *root = parse_object(body);
    const char *message = text_field(field(root, "error"), "message");
    firebase_auth_error_t result = FIREBASE_AUTH_OTHER_ERROR;
    if (message && strcmp(message, "USER_DISABLED") == 0) {
        result = FIREBASE_AUTH_ACCOUNT_DISABLED;
    } else if (message && (strcmp(message, "TOKEN_EXPIRED") == 0 ||
        strcmp(message, "INVALID_REFRESH_TOKEN") == 0 || strcmp(message, "USER_NOT_FOUND") == 0)) {
        result = FIREBASE_AUTH_REFRESH_REJECTED;
    }
    cJSON_Delete(root);
    return result;
}

bool firebase_encode_refresh_form(const char *token, char *out, size_t capacity)
{
    static const char prefix[] = "grant_type=refresh_token&refresh_token=";
    static const char hex[] = "0123456789ABCDEF";
    if (!out || capacity < sizeof(prefix) || !text_valid(token, FIREBASE_REFRESH_TOKEN_BYTES, visible_ascii)) {
        return false;
    }
    memcpy(out, prefix, sizeof(prefix));
    size_t used = sizeof(prefix) - 1;
    for (const char *p = token; *p; ++p) {
        const bool plain = token_char(*p) || *p == '~';
        if (capacity - used <= (plain ? 1U : 3U)) return false;
        if (plain) {
            out[used++] = *p;
        } else {
            out[used++] = '%';
            out[used++] = hex[(unsigned char) *p >> 4U];
            out[used++] = hex[(unsigned char) *p & 15U];
        }
    }
    out[used] = '\0';
    return true;
}

static bool deliver_frame(char *frame, firebase_sse_handler_t handler, void *context)
{
    const char *event = NULL;
    const char *data = NULL;
    for (char *line = frame; *line;) {
        char *next = strchr(line, '\n');
        if (!next) return false;
        char *end = next;
        if (end > line && end[-1] == '\r') --end;
        *end = '\0';
        if (strncmp(line, "event:", 6) == 0) {
            if (event) return false;
            event = line + 6;
            if (*event == ' ') ++event;
        } else if (strncmp(line, "data:", 5) == 0) {
            /* The Realtime Database protocol sends one JSON data line. */
            if (data) return false;
            data = line + 5;
            if (*data == ' ') ++data;
        }
        line = next + 1;
    }
    return !event || handler(event, data, context);
}

bool firebase_sse_feed(firebase_sse_t *stream, const char *bytes, size_t length,
                       firebase_sse_handler_t handler, void *context)
{
    for (size_t i = 0; i < length; ++i) {
        const char c = bytes[i];
        if (c == '\0' || stream->used >= FIREBASE_SSE_FRAME_BYTES) return false;
        stream->buffer[stream->used++] = c;
        if (c != '\n') continue;
        const size_t line_bytes = stream->used - stream->line_start;
        if (line_bytes == 1 || (line_bytes == 2 && stream->buffer[stream->line_start] == '\r')) {
            stream->buffer[stream->line_start] = '\0';
            if (!deliver_frame(stream->buffer, handler, context)) return false;
            stream->used = 0;
        }
        stream->line_start = stream->used;
    }
    return true;
}

static bool parse_command(const cJSON *record, const char *id, const char *session,
                          firebase_command_t *out)
{
    const char *record_id = text_field(record, "id");
    const char *record_session = text_field(record, "sessionId");
    const char *type = text_field(record, "type");
    const cJSON *payload = field(record, "payload");
    const double sequence = cJSON_GetNumberValue(field(record, "sequence"));
    const double issued = cJSON_GetNumberValue(field(record, "issuedAt"));
    const double expires = cJSON_GetNumberValue(field(record, "expiresAt"));
    if (!text_valid(id, sizeof(out->id), path_char) || !record_id || strcmp(record_id, id) != 0 ||
        !record_session || !type ||
        sequence < 1 || sequence > UINT32_MAX || floor(sequence) != sequence ||
        issued < 1700000000000.0 || expires > 9007199254740991.0 ||
        expires <= issued || expires - issued > 30000.0 || floor(issued) != issued || floor(expires) != expires) {
        return false;
    }

    static const struct {
        const char *name;
        firebase_command_type_t type;
        const char *value_field;
    } types[] = {
        {"heat.setTarget", FIREBASE_COMMAND_SET_TARGET, "targetC"},
        {"heat.setRestart", FIREBASE_COMMAND_SET_RESTART, "restartC"},
        {"heat.enable", FIREBASE_COMMAND_ENABLE, NULL},
        {"heat.disable", FIREBASE_COMMAND_DISABLE, NULL},
        {"heat.ackFault", FIREBASE_COMMAND_ACK_FAULT, NULL},
        {"sensor.setCalibration", FIREBASE_COMMAND_CALIBRATE, "calibrationOffsetC"},
        {"printer.monitorStart", FIREBASE_COMMAND_MONITOR_START, NULL},
        {"printer.monitorStop", FIREBASE_COMMAND_MONITOR_STOP, NULL},
        {"firmware.update", FIREBASE_COMMAND_FIRMWARE_UPDATE, NULL},
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        if (strcmp(type, types[i].name) != 0) continue;
        const int session_order = strcmp(record_session, session);
        if (session_order != 0) {
            /* Disable is safe across boots. Reject an advertised future session
             * in this boot and keep all other commands bound to their session. */
            if (types[i].type != FIREBASE_COMMAND_DISABLE ||
                (session_order > 0 &&
                 strncmp(record_session, session, FIREBASE_COMMAND_SESSION_PREFIX_LEN) == 0)) return false;
            const size_t length = strspn(record_session, "0123456789abcdef");
            if (length != FIREBASE_COMMAND_SESSION_BYTES - 1U || record_session[length] != '\0') return false;
        }
        double value = 0;
        if (types[i].value_field) {
            value = cJSON_GetNumberValue(field(payload, types[i].value_field));
            if (!cJSON_IsObject(payload) || cJSON_GetArraySize(payload) != 1 ||
                !isfinite(value) || value < -FLT_MAX || value > FLT_MAX) return false;
        } else if (payload && !cJSON_IsNull(payload) &&
                   (!cJSON_IsObject(payload) || cJSON_GetArraySize(payload) != 0)) {
            return false;
        }
        *out = (firebase_command_t) {.type = types[i].type, .sequence = (uint32_t) sequence,
            .issued_ms = (uint64_t) issued, .expires_ms = (uint64_t) expires, .value = (float) value};
        strcpy(out->id, id);
        return true;
    }
    return false;
}

bool firebase_parse_command_batch(const char *body, const char *session_id, bool *root_snapshot,
                                  firebase_command_t commands[FIREBASE_COMMAND_BATCH_MAX], size_t *count)
{
    *root_snapshot = false;
    *count = 0;
    cJSON *root = parse_object(body);
    const char *path = text_field(root, "path");
    const cJSON *data = field(root, "data");
    bool valid = path && path[0] == '/' && data;
    if (valid && path[1] == '\0') {
        valid = cJSON_IsNull(data) || cJSON_IsObject(data);
        *root_snapshot = valid;
        const cJSON *record = NULL;
        cJSON_ArrayForEach(record, data) {
            if (*count >= FIREBASE_COMMAND_BATCH_MAX) {
                valid = false;
                break;
            }
            if (parse_command(record, record->string, session_id, &commands[*count])) ++*count;
        }
    } else if (valid && parse_command(data, path + 1, session_id, &commands[0])) {
        *count = 1;
    }
    cJSON_Delete(root);
    return valid;
}

bool firebase_command_deadline(const firebase_command_t *command, uint64_t utc_ms,
                               uint64_t monotonic_ms, uint64_t *deadline_ms)
{
    if (utc_ms < UINT64_C(1700000000000) || command->issued_ms > utc_ms + 5000 ||
        command->expires_ms <= utc_ms) return false;
    *deadline_ms = monotonic_ms + command->expires_ms - utc_ms;
    return true;
}

firebase_result_upload_action_t firebase_result_upload_action(int http_status, bool refreshed)
{
    if (http_status == 204) return FIREBASE_RESULT_UPLOADED;
    /* RTDB uses 401 for both an expired token and an expired receipt. */
    if (http_status == 401) return refreshed ? FIREBASE_RESULT_DISCARD : FIREBASE_RESULT_REFRESH_AUTH;
    if (http_status >= 400 && http_status < 500 && http_status != 408 && http_status != 429) {
        return FIREBASE_RESULT_DISCARD;
    }
    return FIREBASE_RESULT_RETRY;
}
