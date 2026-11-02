#include "printer_api_policy.h"
#include "cJSON.h"
#include "device_config.h"
#include <stdio.h>
#include <string.h>
static bool flat(const char *body, size_t n) {
  bool quoted = false;
  unsigned depth = 0;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)body[i];
    if (!c)
      return false;
    if (quoted && c == '\\') {
      if (i + 5 < n && body[i + 1] == 'u' && !memcmp(body + i + 2, "0000", 4))
        return false;
      if (++i >= n)
        return false;
      continue;
    }
    if (c == '"') {
      quoted = !quoted;
      continue;
    }
    if (quoted) {
      if (c < 32)
        return false;
      if (c >= 128) {
        unsigned extra = c >= 0xC2 && c <= 0xDF   ? 1
                         : c >= 0xE0 && c <= 0xEF ? 2
                         : c >= 0xF0 && c <= 0xF4 ? 3
                                                  : 0;
        if (!extra || i + extra >= n)
          return false;
        unsigned cp = c & ((1U << (6 - extra)) - 1);
        for (unsigned k = 0; k < extra; ++k) {
          unsigned char x = (unsigned char)body[++i];
          if ((x & 0xC0) != 0x80)
            return false;
          cp = (cp << 6) | (x & 63);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
          return false;
      }
    } else if (c == '{' || c == '[') {
      if (++depth > 1)
        return false;
    } else if (c == '}' || c == ']') {
      if (!depth)
        return false;
      --depth;
    }
  }
  return !quoted && !depth;
}
static cJSON *parse(const char *body, size_t n) {
  if (!body || !n || n > PRINTER_API_MAX_BODY || !flat(body, n))
    return NULL;
  char buffer[PRINTER_API_MAX_BODY + 1];
  memcpy(buffer, body, n);
  buffer[n] = 0;
  cJSON *root = cJSON_ParseWithLengthOpts(buffer, n + 1, NULL, true);
  if (root && !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    root = NULL;
  }
  return root;
}
static bool text(const cJSON *v, char *out, size_t cap) {
  if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) >= cap)
    return false;
  strcpy(out, v->valuestring);
  return true;
}
bool printer_api_parse_config(const char *body, size_t n,
                              printer_config_request_t *out) {
  if (!out)
    return false;
  cJSON *root = parse(body, n);
  if (!root)
    return false;
  printer_config_request_t result = {.value.ntp_host = "pool.ntp.org"};
  unsigned seen = 0;
  bool valid = true;
  const cJSON *v;
  cJSON_ArrayForEach(v, root) {
    unsigned bit = !strcmp(v->string, "enabled")       ? 1
                   : !strcmp(v->string, "ipv4")        ? 2
                   : !strcmp(v->string, "serial")      ? 4
                   : !strcmp(v->string, "access_code") ? 8
                   : !strcmp(v->string, "ntp_host")    ? 16
                                                       : 0;
    if (!bit || (seen & bit)) {
      valid = false;
      break;
    }
    seen |= bit;
    if (bit == 1) {
      valid = cJSON_IsBool(v);
      result.value.enabled = cJSON_IsTrue(v);
    } else if (bit == 2)
      valid = text(v, result.value.ipv4, sizeof(result.value.ipv4));
    else if (bit == 4)
      valid = text(v, result.value.serial, sizeof(result.value.serial));
    else if (bit == 8) {
      valid =
          text(v, result.value.access_code, sizeof(result.value.access_code));
      result.access_code_present = true;
    } else
      valid = text(v, result.value.ntp_host, sizeof(result.value.ntp_host));
    if (!valid)
      break;
  }
  if ((seen & 7) != 7)
    valid = false;
  printer_connection_config_t check = result.value;
  if (!result.access_code_present)
    strcpy(check.access_code, "validation-only");
  valid = valid && printer_config_valid(&check);
  cJSON_Delete(root);
  if (valid)
    *out = result;
  return valid;
}
bool printer_api_parse_arm(const char *body, size_t n,
                           printer_arm_request_t *out) {
  if (!out)
    return false;
  cJSON *root = parse(body, n);
  if (!root)
    return false;
  printer_arm_request_t result = {0};
  unsigned seen = 0;
  bool valid = true;
  const cJSON *v;
  cJSON_ArrayForEach(v, root) {
    unsigned bit = !strcmp(v->string, "arm_token")                 ? 1
                   : !strcmp(v->string, "initial_pause_confirmed") ? 2
                                                                   : 0;
    if (!bit || (seen & bit)) {
      valid = false;
      break;
    }
    seen |= bit;
    if (bit == 1)
      valid = text(v, result.arm_token, sizeof(result.arm_token));
    else {
      valid = cJSON_IsTrue(v);
      result.initial_pause_confirmed = valid;
    }
    if (!valid)
      break;
  }
  valid = valid && seen == 3 && strlen(result.arm_token) == 32;
  for (unsigned i = 0; valid && i < 32; ++i) {
    char c = result.arm_token[i];
    valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }
  cJSON_Delete(root);
  if (valid)
    *out = result;
  return valid;
}
bool printer_api_empty_body(const char *body, size_t n) {
  if (!n)
    return true;
  cJSON *root = parse(body, n);
  bool valid = root && !root->child;
  cJSON_Delete(root);
  return valid;
}
static const char *run_name(printer_run_state_t state) {
  static const char *names[] = {"unknown", "idle",     "preparing", "running",
                                "paused",  "finished", "failed"};
  return (unsigned)state < sizeof(names) / sizeof(*names) ? names[state]
                                                          : "unknown";
}
static const char *cycle_name(printer_cycle_state_t state) {
  static const char *names[] = {"ready",    "preheating", "resume_pending",
                                "printing", "completed",  "stopped",
                                "error"};
  return (unsigned)state < sizeof(names) / sizeof(*names) ? names[state]
                                                          : "error";
}
static const char *outcome_name(printer_cycle_outcome_t outcome) {
  static const char *names[] = {"none",
                                "completed",
                                "operator_stop",
                                "later_pause",
                                "cancelled",
                                "printer_error",
                                "identity_changed",
                                "telemetry_stale",
                                "link_lost",
                                "heat_fault",
                                "lease_expired",
                                "preheat_timeout",
                                "resume_rejected",
                                "resume_uncertain"};
  return (unsigned)outcome < sizeof(names) / sizeof(*names) ? names[outcome]
                                                            : "unknown";
}
esp_err_t printer_api_status_json(const printer_control_status_t *s,
                                  const char *csrf, char *out, size_t cap) {
  if (!out || !cap)
    return ESP_ERR_INVALID_ARG;
  out[0] = 0;
  if (!s || !csrf)
    return ESP_ERR_INVALID_ARG;
  cJSON *root = cJSON_CreateObject(), *cycle = cJSON_CreateObject();
  if (!root || !cycle) {
    cJSON_Delete(root);
    cJSON_Delete(cycle);
    return ESP_ERR_NO_MEM;
  }
  bool ok = true;
#define STR(o, k, v) (ok = (cJSON_AddStringToObject(o, k, v) != NULL) && ok)
#define BOOL(o, k, v) (ok = (cJSON_AddBoolToObject(o, k, v) != NULL) && ok)
#define NUM(o, k, v) (ok = (cJSON_AddNumberToObject(o, k, v) != NULL) && ok)
  BOOL(root, "configured", s->configured);
  BOOL(root, "enabled", s->enabled);
  BOOL(root, "access_code_set", s->access_code_set);
  BOOL(root, "applying", s->applying);
  STR(root, "ipv4", s->ipv4);
  STR(root, "serial", s->serial);
  STR(root, "ntp_host", s->ntp_host);
  STR(root, "config_result", s->config_result);
  STR(root, "reason", s->reason);
  BOOL(root, "can_arm", s->can_arm);
  STR(root, "arm_token", s->can_arm ? s->cycle.arm_token : "");
  NUM(root, "token_expires_ms", s->cycle.token_expires_ms);
  BOOL(root, "connected", s->observation.connected);
  BOOL(root, "fresh", s->observation.fresh);
  BOOL(root, "baseline_valid", s->observation.baseline_valid);
  STR(root, "printer_state", run_name(s->observation.state));
  STR(root, "job_label",
      s->observation.job.label[0] ? s->observation.job.label
                                  : s->observation.job.file);
  if (s->observation.progress_valid)
    NUM(root, "progress", s->observation.progress);
  else
    ok = (cJSON_AddNullToObject(root, "progress") != NULL) && ok;
  if (s->observation.remaining_valid)
    NUM(root, "remaining_minutes", s->observation.remaining);
  else
    ok = (cJSON_AddNullToObject(root, "remaining_minutes") != NULL) && ok;
  char id[21];
  snprintf(id, sizeof(id), "%llu", (unsigned long long)s->cycle.cycle_id);
  STR(cycle, "id", id);
  STR(cycle, "state", cycle_name(s->cycle.state));
  STR(cycle, "outcome", outcome_name(s->cycle.outcome));
  NUM(cycle, "target_c", s->cycle.target_c);
  BOOL(cycle, "resume_attempted", s->cycle.resume_attempted);
  if (!cJSON_AddItemToObject(root, "cycle", cycle)) {
    cJSON_Delete(cycle);
    ok = false;
  }
  STR(root, "csrf_token", csrf);
#undef STR
#undef BOOL
#undef NUM
  bool printed =
      ok && cJSON_PrintPreallocated(root, out,
                                    (int)(cap > PRINTER_API_MAX_RESPONSE
                                              ? PRINTER_API_MAX_RESPONSE
                                              : cap),
                                    false);
  cJSON_Delete(root);
  if (!printed)
    out[0] = 0;
  return !ok ? ESP_ERR_NO_MEM : printed ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
