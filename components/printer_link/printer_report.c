#include "printer_report.h"
#include <ctype.h>
#include <string.h>

/* Streaming validation/projection: bounded recursion, no heap or token tree. */
typedef struct {
  const char *s, *end;
  printer_report_t r;
  bool print_seen, command_ok;
} parser_t;
static void ws(parser_t *p) {
  while (p->s < p->end && strchr(" \t\r\n", *p->s))
    p->s++;
}
static bool take(parser_t *p, char c) {
  ws(p);
  if (p->s == p->end || *p->s != c)
    return false;
  p->s++;
  return true;
}
static bool hex4(parser_t *p, uint32_t *out) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; i++) {
    if (p->s == p->end)
      return false;
    unsigned char c = (unsigned char)*p->s++;
    unsigned n = c >= '0' && c <= '9'   ? c - '0'
                 : c >= 'a' && c <= 'f' ? c - 'a' + 10
                 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                        : 99;
    if (n > 15)
      return false;
    v = v * 16 + n;
  }
  *out = v;
  return true;
}
static bool string(parser_t *p, char *out, size_t cap) {
  if (!take(p, '"'))
    return false;
  size_t used = 0;
  while (p->s < p->end && *p->s != '"') {
    uint32_t cp = (unsigned char)*p->s++;
    if (cp < 32)
      return false;
    if (cp == '\\') {
      if (p->s == p->end)
        return false;
      char e = *p->s++;
      if (e == 'u') {
        if (!hex4(p, &cp))
          return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          uint32_t lo;
          if (p->end - p->s < 2 || p->s[0] != '\\' || p->s[1] != 'u')
            return false;
          p->s += 2;
          if (!hex4(p, &lo) || lo < 0xDC00 || lo > 0xDFFF)
            return false;
          cp = 0x10000 + ((cp - 0xD800) << 10) + lo - 0xDC00;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF)
          return false;
      } else {
        const char *esc = "\"\\/bfnrt", *found = strchr(esc, e);
        if (!found)
          return false;
        cp = (unsigned char)"\"\\/\b\f\n\r\t"[found - esc];
      }
    } else if (cp >= 128) {
      unsigned n = cp >= 0xC2 && cp <= 0xDF   ? 1
                   : cp >= 0xE0 && cp <= 0xEF ? 2
                   : cp >= 0xF0 && cp <= 0xF4 ? 3
                                              : 99;
      if (n == 99 || p->end - p->s < (ptrdiff_t)n)
        return false;
      uint32_t min = n == 1 ? 0x80 : n == 2 ? 0x800 : 0x10000;
      cp &= (1U << (6U - n)) - 1U;
      for (unsigned i = 0; i < n; i++) {
        unsigned char c = (unsigned char)*p->s++;
        if ((c & 0xC0) != 0x80)
          return false;
        cp = (cp << 6) | (c & 63);
      }
      if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return false;
    }
    if (cp == 0)
      return false;
    unsigned char b[4];
    size_t n;
    if (cp < 128) {
      b[0] = (unsigned char)cp;
      n = 1;
    } else if (cp < 0x800) {
      b[0] = 0xC0 | (cp >> 6);
      b[1] = 0x80 | (cp & 63);
      n = 2;
    } else if (cp < 0x10000) {
      b[0] = 0xE0 | (cp >> 12);
      b[1] = 0x80 | ((cp >> 6) & 63);
      b[2] = 0x80 | (cp & 63);
      n = 3;
    } else {
      b[0] = 0xF0 | (cp >> 18);
      b[1] = 0x80 | ((cp >> 12) & 63);
      b[2] = 0x80 | ((cp >> 6) & 63);
      b[3] = 0x80 | (cp & 63);
      n = 4;
    }
    if (out) {
      if (used + n >= cap)
        return false;
      memcpy(out + used, b, n);
    }
    used += n;
  }
  if (p->s == p->end)
    return false;
  p->s++;
  if (out)
    out[used] = 0;
  return true;
}
static bool number(parser_t *p, char *out, size_t cap) {
  ws(p);
  const char *start = p->s;
  if (p->s < p->end && *p->s == '-')
    p->s++;
  if (p->s == p->end)
    return false;
  if (*p->s == '0')
    p->s++;
  else {
    if (*p->s < '1' || *p->s > '9')
      return false;
    while (p->s < p->end && isdigit((unsigned char)*p->s))
      p->s++;
  }
  if (p->s < p->end && *p->s == '.') {
    p->s++;
    const char *digits = p->s;
    while (p->s < p->end && isdigit((unsigned char)*p->s))
      p->s++;
    if (p->s == digits)
      return false;
  }
  if (p->s < p->end && (*p->s == 'e' || *p->s == 'E')) {
    p->s++;
    if (p->s < p->end && (*p->s == '+' || *p->s == '-'))
      p->s++;
    const char *digits = p->s;
    while (p->s < p->end && isdigit((unsigned char)*p->s))
      p->s++;
    if (p->s == digits)
      return false;
  }
  size_t n = (size_t)(p->s - start);
  if (out) {
    if (n >= cap)
      return false;
    memcpy(out, start, n);
    out[n] = 0;
  }
  return true;
}
static bool value(parser_t *p, unsigned depth);
static bool literal(parser_t *p, const char *s) {
  size_t n = strlen(s);
  if ((size_t)(p->end - p->s) < n || memcmp(p->s, s, n))
    return false;
  p->s += n;
  return true;
}
static bool value(parser_t *p, unsigned depth) {
  ws(p);
  if (depth > 16 || p->s == p->end)
    return false;
  if (*p->s == '"')
    return string(p, NULL, 0);
  if (*p->s == '{' || *p->s == '[') {
    char open = *p->s++, end = open == '{' ? '}' : ']';
    ws(p);
    if (p->s < p->end && *p->s == end) {
      p->s++;
      return true;
    }
    do {
      if (open == '{' && (!string(p, NULL, 0) || !take(p, ':')))
        return false;
      if (!value(p, depth + 1))
        return false;
      ws(p);
      if (p->s < p->end && *p->s == end) {
        p->s++;
        return true;
      }
    } while (take(p, ','));
    return false;
  }
  if (*p->s == 't')
    return literal(p, "true");
  if (*p->s == 'f')
    return literal(p, "false");
  if (*p->s == 'n')
    return literal(p, "null");
  return number(p, NULL, 0);
}
static printer_run_state_t state(const char *s) {
  if (!strcmp(s, "IDLE"))
    return PRINTER_RUN_IDLE;
  if (!strcmp(s, "RUNNING"))
    return PRINTER_RUN_RUNNING;
  if (!strcmp(s, "PREPARE") || !strcmp(s, "SLICING"))
    return PRINTER_RUN_PREPARING;
  if (!strcmp(s, "PAUSE"))
    return PRINTER_RUN_PAUSED;
  if (!strcmp(s, "FINISH"))
    return PRINTER_RUN_FINISHED;
  if (!strcmp(s, "FAILED"))
    return PRINTER_RUN_FAILED;
  return PRINTER_RUN_UNKNOWN;
}
/* Validates the whole print object; keeps only gcode_state. */
static bool print_object(parser_t *p) {
  if (!take(p, '{'))
    return false;
  ws(p);
  if (take(p, '}'))
    return true;
  bool command_seen = false;
  do {
    char key[64];
    if (!string(p, key, sizeof(key)) || !take(p, ':'))
      return false;
    if (!strcmp(key, "gcode_state")) {
      char s[48];
      if (p->r.has_state || !string(p, s, sizeof(s)))
        return false;
      p->r.has_state = true;
      p->r.state = state(s);
    } else if (!strcmp(key, "command")) {
      char cmd[48];
      if (command_seen || !string(p, cmd, sizeof(cmd)))
        return false;
      command_seen = true;
      p->command_ok = !strcmp(cmd, "push_status");
    } else if (!value(p, 2)) {
      return false;
    }
    ws(p);
    if (take(p, '}'))
      return true;
  } while (take(p, ','));
  return false;
}
esp_err_t printer_report_parse(const char *json, size_t len,
                               printer_report_t *out) {
  if (!json || !out || !len || len > PRINTER_REPORT_MAX || memchr(json, 0, len))
    return ESP_ERR_INVALID_ARG;
  parser_t p = {.s = json, .end = json + len, .command_ok = true};
  if (!take(&p, '{'))
    return ESP_ERR_INVALID_RESPONSE;
  if (!take(&p, '}'))
    do {
      char key[64];
      if (!string(&p, key, sizeof(key)) || !take(&p, ':'))
        return ESP_ERR_INVALID_RESPONSE;
      if (!strcmp(key, "print")) {
        if (p.print_seen || !print_object(&p))
          return ESP_ERR_INVALID_RESPONSE;
        p.print_seen = true;
      } else if (!value(&p, 1))
        return ESP_ERR_INVALID_RESPONSE;
      if (take(&p, '}'))
        break;
      if (!take(&p, ','))
        return ESP_ERR_INVALID_RESPONSE;
    } while (true);
  ws(&p);
  if (p.s != p.end)
    return ESP_ERR_INVALID_RESPONSE;
  if (!p.print_seen || !p.command_ok)
    return ESP_ERR_NOT_FOUND;
  *out = p.r;
  return ESP_OK;
}
printer_frame_result_t printer_frame_feed(printer_frame_t *f,
                                          const printer_fragment_t *p,
                                          const char **json, size_t *len) {
  if (!f || !p || !json || !len)
    return PRINTER_FRAME_REJECTED;
  *json = NULL;
  *len = 0;
  if (p->retained || !p->data || !p->len || !p->total ||
      p->total > PRINTER_REPORT_MAX || p->offset > p->total ||
      p->len > p->total - p->offset) {
    f->active = false;
    return PRINTER_FRAME_REJECTED;
  }
  if (p->offset == 0) {
    if (!p->topic || p->topic_len < 8 ||
        memcmp(p->topic + p->topic_len - 7, "/report", 7)) {
      f->active = false;
      return PRINTER_FRAME_REJECTED;
    }
    f->active = true;
    f->used = 0;
    f->total = p->total;
    f->epoch = p->epoch;
  }
  if (!f->active || p->epoch != f->epoch || p->offset != f->used ||
      p->total != f->total) {
    f->active = false;
    return PRINTER_FRAME_REJECTED;
  }
  memcpy(f->bytes + f->used, p->data, p->len);
  f->used += p->len;
  if (f->used != f->total)
    return PRINTER_FRAME_PENDING;
  f->bytes[f->used] = 0;
  f->active = false;
  *json = f->bytes;
  *len = f->used;
  return PRINTER_FRAME_COMPLETE;
}
