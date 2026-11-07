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
  printer_config_request_t result = {0};
  unsigned seen = 0;
  bool valid = true;
  const cJSON *v;
  cJSON_ArrayForEach(v, root) {
    unsigned bit = !strcmp(v->string, "enabled")       ? 1
                   : !strcmp(v->string, "ipv4")        ? 2
                   : !strcmp(v->string, "serial")      ? 4
                   : !strcmp(v->string, "access_code") ? 8
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
    }
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
static const char *phase_name(printer_monitor_phase_t phase) {
  static const char *names[] = {"off", "waiting", "watching"};
  return (unsigned)phase < sizeof(names) / sizeof(*names) ? names[phase] : "off";
}
static const char *hold_name(printer_hold_t hold) {
  static const char *names[] = {"none", "paused", "disconnected", "unknown"};
  return (unsigned)hold < sizeof(names) / sizeof(*names) ? names[hold] : "unknown";
}
static const char *end_name(printer_monitor_end_t end) {
  static const char *names[] = {"none",     "finished", "failed",      "idle",
                                "no_print", "stopped",  "heating_off", "settings"};
  return (unsigned)end < sizeof(names) / sizeof(*names) ? names[end] : "none";
}
esp_err_t printer_api_status_json(const printer_control_status_t *s,
                                  const char *csrf, char *out, size_t cap) {
  if (!out || !cap)
    return ESP_ERR_INVALID_ARG;
  out[0] = 0;
  if (!s || !csrf)
    return ESP_ERR_INVALID_ARG;
  cJSON *root = cJSON_CreateObject(), *monitor = cJSON_CreateObject();
  if (!root || !monitor) {
    cJSON_Delete(root);
    cJSON_Delete(monitor);
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
  STR(root, "config_result", s->config_result);
  STR(root, "reason", s->reason);
  BOOL(root, "can_monitor", s->can_monitor);
  BOOL(root, "connected", s->link.connected);
  STR(root, "printer_state",
      s->link.state_known ? run_name(s->link.state) : "unknown");
  NUM(root, "reconnects", s->link.reconnects);
  STR(monitor, "phase", phase_name(s->monitor.phase));
  STR(monitor, "hold", hold_name(s->monitor.hold));
  STR(monitor, "last_end", end_name(s->monitor.last_end));
  NUM(monitor, "wait_remaining_ms", (double)s->wait_remaining_ms);
  if (!cJSON_AddItemToObject(root, "monitor", monitor)) {
    cJSON_Delete(monitor);
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
