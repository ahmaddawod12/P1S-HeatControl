#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "firebase_protocol.h"

static void test_pairing(void)
{
    firebase_pairing_session_t session;
    assert(firebase_parse_pairing_session("{\"code\":\"P1S7E4\","
        "\"sessionSecret\":\"abcdefghijklmnopqrstuvwxyz012345\",\"expiresIn\":300}", &session));
    assert(strcmp(session.code, "P1S7E4") == 0);
    assert(session.expires_in == 300);
    assert(!firebase_parse_pairing_session("{\"code\":\"P1S7E4\","
        "\"sessionSecret\":\"short\",\"expiresIn\":300}", &session));
    assert(!firebase_parse_pairing_session("{\"code\":\"P1S7E4\",\"code\":\"DUPLICATE\","
        "\"sessionSecret\":\"abcdefghijklmnopqrstuvwxyz012345\",\"expiresIn\":300}", &session));
    assert(strcmp(session.code, "P1S7E4") == 0);

    firebase_pairing_claim_t claim;
    assert(firebase_parse_pairing_claim("{\"deviceUid\":\"p1s-device\",\"customToken\":\"header.body.signature\"}", &claim));
    assert(strcmp(claim.uid, "p1s-device") == 0);
    assert(!firebase_parse_pairing_claim("{\"deviceUid\":\"p1s/device\",\"customToken\":\"jwt\"}", &claim));
    assert(!firebase_parse_pairing_claim("{\"deviceUid\":\"p1s-device\",\"customToken\":\"a&b\"}", &claim));

    const char denied[] = "{\"error\":\"permission-denied\"}";
    assert(firebase_pairing_recovery_rejected(403, denied));
    assert(!firebase_pairing_recovery_rejected(503, denied));
    assert(!firebase_pairing_recovery_rejected(429, "{\"error\":\"resource-exhausted\"}"));
    assert(!firebase_pairing_recovery_rejected(403, "<html>Forbidden</html>"));
    const char *disabled = "{\"error\":\"device_disabled\"}";
    assert(firebase_pairing_account_disabled(409, disabled));
    assert(!firebase_pairing_account_disabled(403, disabled));
    assert(!firebase_pairing_account_disabled(409, denied));
}

static void test_tokens(void)
{
    firebase_auth_tokens_t tokens;
    assert(firebase_parse_auth_tokens("{\"idToken\":\"header.payload.sig\","
        "\"refreshToken\":\"device+token/=%\",\"expiresIn\":\"3600\"}", false, "p1s-device", &tokens));
    assert(tokens.expires_in == 3600);
    char body[64];
    assert(firebase_encode_refresh_form(tokens.refresh_token, body, sizeof(body)));
    assert(strcmp(body, "grant_type=refresh_token&refresh_token=device%2Btoken%2F%3D%25") == 0);
    assert(!firebase_encode_refresh_form(tokens.refresh_token, body, 42));
    const char reply[] = "{\"id_token\":\"new.jwt\",\"refresh_token\":\"rotated\","
        "\"expires_in\":\"3600\",\"user_id\":\"p1s-device\"}";
    assert(firebase_parse_auth_tokens(reply, true, "p1s-device", &tokens));
    assert(strcmp(tokens.refresh_token, "rotated") == 0);
    assert(!firebase_parse_auth_tokens(reply, true, "another-device", &tokens));
    assert(!firebase_parse_auth_tokens("{\"idToken\":\"x\\u0000y\","
        "\"refreshToken\":\"t\",\"expiresIn\":\"3600\"}", false, "p1s-device", &tokens));
    assert(!firebase_parse_auth_tokens("{\"idToken\":\"x\",\"refreshToken\":\"t\","
        "\"expiresIn\":\"3600oops\"}", false, "p1s-device", &tokens));
    assert(firebase_auth_error("{\"error\":{\"message\":\"INVALID_REFRESH_TOKEN\"}}") == FIREBASE_AUTH_REFRESH_REJECTED);
    assert(firebase_auth_error("{\"error\":{\"message\":\"USER_DISABLED\"}}") == FIREBASE_AUTH_ACCOUNT_DISABLED);
    assert(firebase_auth_error("{\"error\":{\"message\":\"PROJECT_NUMBER_MISMATCH\"}}") == FIREBASE_AUTH_OTHER_ERROR);
    assert(firebase_auth_error("service unavailable") == FIREBASE_AUTH_OTHER_ERROR);
}

static void test_urls(void)
{
    assert(firebase_database_url_valid("https://p1s-default-rtdb.firebaseio.com"));
    assert(firebase_database_url_valid("https://p1s-default-rtdb.europe-west1.firebasedatabase.app"));
    assert(!firebase_database_url_valid("http://p1s-default-rtdb.firebaseio.com"));
    assert(!firebase_database_url_valid("https://p1s.firebaseio.com.example.invalid"));
    assert(!firebase_database_url_valid("https://p1s.firebaseio.com/devices"));
    assert(!firebase_database_url_valid("https://p1s.firebaseio.com:443"));
    assert(firebase_database_redirect_valid("https://server.firebaseio.com/devices/board/commands.json?ns=project"));
    assert(firebase_database_redirect_valid("https://server.firebaseio.com:443/devices/board/commands.json"));
    assert(firebase_database_redirect_valid("https://server.firebasedatabase.app:443/devices/board/commands.json"));
    assert(!firebase_database_redirect_valid("https://server.firebaseio.com:4430/devices"));
    assert(!firebase_database_redirect_valid("https://server.firebaseio.com:443@outside.invalid/devices"));
    assert(!firebase_database_redirect_valid("https://p1s.firebaseio.com@example.invalid/commands.json"));
    assert(firebase_pairing_url_valid("https://pair.example.invalid/api/pair"));
    assert(!firebase_pairing_url_valid("https://pair.example.invalid/api/pair?token=key"));
    assert(!firebase_pairing_url_valid("http://pair.example.invalid/api/pair"));
}

static void test_commands(void)
{
    const char command[] = "{\"path\":\"/command-A\",\"data\":{\"id\":\"command-A\","
        "\"sessionId\":\"session-1\",\"sequence\":42,\"type\":\"heat.setTarget\","
        "\"payload\":{\"targetC\":60},\"issuedAt\":1750000000000,\"expiresAt\":1750000030000}}";
    firebase_command_t values[FIREBASE_COMMAND_BATCH_MAX];
    size_t count;
    bool snapshot;
    assert(firebase_parse_command_batch("{\"path\":\"/\",\"data\":null}", "session-1", &snapshot, values, &count));
    assert(snapshot && count == 0);
    assert(firebase_parse_command_batch(command, "session-2", &snapshot, values, &count));
    assert(!snapshot && count == 0);
    assert(firebase_parse_command_batch(command, "session-1", &snapshot, values, &count));
    assert(count == 1 && values[0].type == FIREBASE_COMMAND_SET_TARGET && values[0].value == 60.0f);
    assert(values[0].sequence == 42 && strcmp(values[0].id, "command-A") == 0);
    uint64_t deadline;
    assert(firebase_command_deadline(&values[0], UINT64_C(1750000010000), 20, &deadline));
    assert(deadline == 20020);
    assert(!firebase_command_deadline(&values[0], UINT64_C(1750000030000), 20, &deadline));
    assert(!firebase_command_deadline(&values[0], 0, 20, &deadline));
    assert(!firebase_command_deadline(&values[0], UINT64_C(1749999994000), 20, &deadline));
    assert(firebase_parse_command_batch("{\"path\":\"/A\",\"data\":{\"id\":\"B\","
        "\"sessionId\":\"session-1\",\"sequence\":43,\"type\":\"heat.enable\","
        "\"issuedAt\":1750000000000,\"expiresAt\":1750000030000}}", "session-1", &snapshot, values, &count));
    assert(count == 0);
}

static bool check_event(const char *type, const char *data, void *context)
{
    unsigned *received = context;
    if (strcmp(type, "put") == 0) {
        assert(strcmp(data, "{\"path\":\"/\",\"data\":null}") == 0);
    } else if (strcmp(type, "auth_revoked") == 0) {
        return false;
    } else {
        assert(strcmp(type, "keep-alive") == 0 && strcmp(data, "null") == 0);
    }
    ++*received;
    return true;
}

static void test_stream(void)
{
    firebase_sse_t parser = {0};
    unsigned received = 0;
    const char first[] = "event: pu";
    const char second[] = "t\r\ndata: {\"path\":\"/\",\"data\":null}\r\n\r\nevent: keep-alive\ndata: null\n\n";
    const char revoked[] = "event: auth_revoked\ndata: \"expired\"\n\n";
    assert(firebase_sse_feed(&parser, first, sizeof(first) - 1, check_event, &received));
    assert(received == 0);
    assert(firebase_sse_feed(&parser, second, sizeof(second) - 1, check_event, &received));
    assert(received == 2);
    assert(!firebase_sse_feed(&parser, revoked, sizeof(revoked) - 1, check_event, &received));
    parser = (firebase_sse_t) {0};
    assert(!firebase_sse_feed(&parser, "", 1, check_event, &received));
}

int main(void)
{
    test_pairing();
    test_tokens();
    test_urls();
    test_commands();
    test_stream();
    assert(strcmp(firebase_heat_state_name(HEAT_STATE_HEATING), "heating") == 0);
    assert(strcmp(firebase_heat_fault_name(HEAT_FAULT_OVERTEMP), "overtemperature") == 0);
    assert(firebase_result_upload_action(204, false) == FIREBASE_RESULT_UPLOADED);
    assert(firebase_result_upload_action(401, false) == FIREBASE_RESULT_REFRESH_AUTH);
    assert(firebase_result_upload_action(401, true) == FIREBASE_RESULT_DISCARD);
    assert(firebase_result_upload_action(403, false) == FIREBASE_RESULT_DISCARD);
    assert(firebase_result_upload_action(429, true) == FIREBASE_RESULT_RETRY);
    assert(firebase_result_upload_action(503, false) == FIREBASE_RESULT_RETRY);
    puts("Firebase protocol passed");
    return 0;
}
