#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_types.h"

#define FIREBASE_UID_BYTES 129U
#define FIREBASE_ID_TOKEN_BYTES 2048U
#define FIREBASE_REFRESH_TOKEN_BYTES 1024U
#define FIREBASE_CUSTOM_TOKEN_BYTES 2048U
#define FIREBASE_PAIRING_CODE_BYTES 17U
#define FIREBASE_PAIRING_SECRET_BYTES 129U
#define FIREBASE_COMMAND_ID_BYTES 65U
#define FIREBASE_COMMAND_SESSION_BYTES 33U
/* The prefix identifies the boot and device UID across stream reconnects. */
#define FIREBASE_COMMAND_SESSION_PREFIX_LEN 24U
#define FIREBASE_COMMAND_BATCH_MAX 12U
#define FIREBASE_SSE_FRAME_BYTES 6144U

typedef struct {
    char id_token[FIREBASE_ID_TOKEN_BYTES];
    char refresh_token[FIREBASE_REFRESH_TOKEN_BYTES];
    uint32_t expires_in;
} firebase_auth_tokens_t;

typedef struct {
    char code[FIREBASE_PAIRING_CODE_BYTES];
    char secret[FIREBASE_PAIRING_SECRET_BYTES];
    uint32_t expires_in;
} firebase_pairing_session_t;

typedef struct {
    char uid[FIREBASE_UID_BYTES];
    char custom_token[FIREBASE_CUSTOM_TOKEN_BYTES];
} firebase_pairing_claim_t;

typedef enum {
    FIREBASE_COMMAND_SET_TARGET,
    FIREBASE_COMMAND_ENABLE,
    FIREBASE_COMMAND_DISABLE,
    FIREBASE_COMMAND_ACK_FAULT,
    FIREBASE_COMMAND_CALIBRATE,
    FIREBASE_COMMAND_MONITOR_START,
    FIREBASE_COMMAND_MONITOR_STOP,
    FIREBASE_COMMAND_SET_RESTART,
    FIREBASE_COMMAND_FIRMWARE_UPDATE,
} firebase_command_type_t;

typedef struct {
    char id[FIREBASE_COMMAND_ID_BYTES];
    firebase_command_type_t type;
    uint32_t sequence;
    uint64_t issued_ms, expires_ms;
    float value;
} firebase_command_t;

typedef struct {
    char buffer[FIREBASE_SSE_FRAME_BYTES + 1];
    size_t used, line_start;
} firebase_sse_t;

typedef bool (*firebase_sse_handler_t)(const char *event, const char *data, void *context);

bool firebase_uid_valid(const char *uid);
const char *firebase_heat_state_name(heat_state_t state);
const char *firebase_heat_fault_name(heat_fault_t fault);
bool firebase_database_url_valid(const char *url);
bool firebase_database_redirect_valid(const char *url);
bool firebase_pairing_url_valid(const char *url);
bool firebase_parse_pairing_session(const char *body, firebase_pairing_session_t *out);
bool firebase_parse_pairing_claim(const char *body, firebase_pairing_claim_t *out);
bool firebase_pairing_recovery_rejected(int status, const char *body);
bool firebase_pairing_account_disabled(int status, const char *body);
bool firebase_parse_auth_tokens(const char *body, bool refresh, const char *expected_uid,
                               firebase_auth_tokens_t *out);
typedef enum {
    FIREBASE_AUTH_OTHER_ERROR,
    FIREBASE_AUTH_REFRESH_REJECTED,
    FIREBASE_AUTH_ACCOUNT_DISABLED,
} firebase_auth_error_t;
firebase_auth_error_t firebase_auth_error(const char *body);
bool firebase_encode_refresh_form(const char *token, char *out, size_t capacity);
bool firebase_sse_feed(firebase_sse_t *stream, const char *bytes, size_t length,
                       firebase_sse_handler_t handler, void *context);
bool firebase_parse_command_batch(const char *body, const char *session_id, bool *root_snapshot,
                                  firebase_command_t commands[FIREBASE_COMMAND_BATCH_MAX], size_t *count);
bool firebase_command_deadline(const firebase_command_t *command, uint64_t utc_ms,
                               uint64_t monotonic_ms, uint64_t *deadline_ms);

typedef enum {
    FIREBASE_RESULT_UPLOADED,
    FIREBASE_RESULT_REFRESH_AUTH,
    FIREBASE_RESULT_RETRY,
    FIREBASE_RESULT_DISCARD,
} firebase_result_upload_action_t;

firebase_result_upload_action_t firebase_result_upload_action(int http_status, bool refreshed);
