#pragma once

#include "esp_err.h"
#include "firebase_protocol.h"

typedef struct {
    bool ready;
    char session_id[FIREBASE_COMMAND_SESSION_BYTES];
    uint64_t last_activity_ms;
} firebase_command_status_t;

typedef struct {
    char uid[FIREBASE_UID_BYTES];
    char id[FIREBASE_COMMAND_ID_BYTES];
    const char *status;
    const char *reason;
    uint64_t deliver_until_ms;
} firebase_command_result_t;

esp_err_t firebase_commands_start(void);
/* Start the command workers before publishing authentication. Keep the
 * controller UID while its token is unavailable to retain a received Disable. */
void firebase_commands_set_auth(const char *uid, const char *id_token);
void firebase_commands_get_status(firebase_command_status_t *out);
bool firebase_commands_peek_result(firebase_command_result_t *out);
void firebase_commands_remove_result(void);
