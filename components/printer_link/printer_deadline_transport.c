#include "printer_deadline_transport.h"
#include <stdlib.h>
typedef struct {
  esp_transport_handle_t inner;
  uint64_t (*now)(void *);
  bool (*allowed)(void *);
  void *ctx;
  uint64_t deadline;
  void (*cleanup)(void *);
  void *cleanup_ctx;
} bounded_t;
static bounded_t *get(esp_transport_handle_t t) {
  return esp_transport_get_context_data(t);
}
static int connect_wire(esp_transport_handle_t t, const char *h, int port,
                        int timeout) {
  bounded_t *b = get(t);
  int result = esp_transport_connect(b->inner, h, port, timeout);
  /* TLS writes must return WANT_READ/WRITE, never block past the outer budget.
   * This runs in the MQTT task, before any application publish can be admitted.
   */
  if (result == 0 &&
      !printer_socket_nonblocking(esp_transport_get_socket(b->inner))) {
    esp_transport_close(b->inner);
    return -1;
  }
  return result;
}
static int read_wire(esp_transport_handle_t t, char *data, int len, int ms) {
  return esp_transport_read(get(t)->inner, data, len, ms);
}
static int write_wire(esp_transport_handle_t t, const char *data, int len,
                      int ms) {
  bounded_t *b = get(t);
  if (b->deadline) {
    for (;;) {
      if (!b->allowed(b->ctx))
        return -1;
      uint64_t now = b->now(b->ctx);
      if (now >= b->deadline)
        return -1;
      uint64_t left = b->deadline - now;
      int poll =
          esp_transport_poll_write(b->inner, (int)(left > 10 ? 10 : left));
      if (poll < 0)
        return -1;
      if (!poll)
        continue;
      /* Stop/fault may arrive during select. Never carry permission across it.
       */
      if (!b->allowed(b->ctx) || b->now(b->ctx) >= b->deadline)
        return -1;
      return esp_transport_write(b->inner, data, len, 0);
    }
  }
  /* Rechecked for every partial MQTT write, using one absolute deadline. */
  return esp_transport_write(b->inner, data, len, ms);
}
static int close_wire(esp_transport_handle_t t) {
  return esp_transport_close(get(t)->inner);
}
static int poll_read(esp_transport_handle_t t, int ms) {
  return esp_transport_poll_read(get(t)->inner, ms);
}
static int poll_write(esp_transport_handle_t t, int ms) {
  return esp_transport_poll_write(get(t)->inner, ms);
}
static int destroy_wire(esp_transport_handle_t t) {
  bounded_t *b = get(t);
  esp_transport_destroy(b->inner);
  if (b->cleanup)
    b->cleanup(b->cleanup_ctx);
  free(b);
  return 0;
}
esp_transport_handle_t
printer_deadline_transport_create(esp_transport_handle_t inner,
                                  uint64_t (*now)(void *),
                                  bool (*allowed)(void *), void *ctx) {
  if (!inner || !now || !allowed)
    return NULL;
  bounded_t *b = calloc(1, sizeof(*b));
  esp_transport_handle_t t = esp_transport_init();
  if (!b || !t) {
    free(b);
    if (t)
      esp_transport_destroy(t);
    esp_transport_destroy(inner);
    return NULL;
  }
  *b = (bounded_t){.inner = inner, .now = now, .allowed = allowed, .ctx = ctx};
  esp_transport_set_context_data(t, b);
  esp_transport_set_default_port(t, 8883);
  esp_transport_set_func(t, connect_wire, read_wire, write_wire, close_wire,
                         poll_read, poll_write, destroy_wire);
  return t;
}
void printer_deadline_transport_set(esp_transport_handle_t t,
                                    uint64_t deadline) {
  get(t)->deadline = deadline;
}
void printer_deadline_transport_cleanup(esp_transport_handle_t t,
                                        void (*fn)(void *), void *ctx) {
  get(t)->cleanup = fn;
  get(t)->cleanup_ctx = ctx;
}
