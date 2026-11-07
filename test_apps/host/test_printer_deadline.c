#include "printer_deadline_transport.h"
#include <assert.h>
#include <string.h>
static uint64_t now;
static bool permitted = true;
static unsigned writes;
static unsigned destroyed;
static unsigned poll_mode;
static bool slow_guard;
static int socket_flags = 0x40;
static bool fail_setup, fail_restore, fail_write;
static unsigned closes, setup_mode;
static char incoming[4098];
static size_t incoming_offset;
/* The SDK's TLS read returns WANT_READ (mapped to zero) when a later TLS
 * record is incomplete on a nonblocking socket. Blocking mode completes it. */
static int fragment_read(esp_transport_handle_t t, char *out, int len, int ms) {
  (void)t;
  assert(ms == 0 || ms == 10000);
  if (socket_flags & 1) return 0;
  size_t left = sizeof(incoming) - incoming_offset;
  size_t count = (size_t)len < left ? (size_t)len : left;
  if (count > 701) count = 701;
  memcpy(out, incoming + incoming_offset, count);
  incoming_offset += count;
  return (int)count;
}
bool printer_socket_nonblocking_begin(int fd, int *saved) {
  assert(fd == 42 && saved);
  if (fail_setup) return false;
  *saved = socket_flags;
  socket_flags |= 1;
  if (setup_mode == 1) permitted = false;
  if (setup_mode == 2) now += 1000;
  return true;
}
bool printer_socket_restore(int fd, int saved) {
  assert(fd == 42);
  if (fail_restore) return false;
  socket_flags = saved;
  return true;
}
static int close_inner(esp_transport_handle_t t) { (void)t; ++closes; return 0; }
static void cleanup(void *ctx) {
  (void)ctx;
  ++destroyed;
}
static uint64_t clock_now(void *c) {
  (void)c;
  return now;
}
static bool allowed(void *c) {
  (void)c;
  if (slow_guard)
    now += 1000;
  return permitted;
}
static int poll(esp_transport_handle_t t, int ms) {
  (void)t;
  assert(ms > 0 && ms <= 10);
  now += (unsigned)ms;
  if (poll_mode == 2)
    permitted = false;
  return poll_mode == 1 ? 0 : 1;
}
static int partial_write(esp_transport_handle_t t, const char *b, int n,
                         int ms) {
  (void)t;
  (void)b;
  assert(n > 0 && ms == 0);
  assert((socket_flags & 1) != 0);
  ++writes;
  return fail_write ? -1 : 1;
}
int main(void) {
  esp_transport_handle_t inner = esp_transport_init();
  esp_transport_set_func(inner, 0, fragment_read, partial_write, close_inner, 0, poll, 0);
  esp_transport_handle_t outer =
      printer_deadline_transport_create(inner, clock_now, allowed, 0);
  assert(outer);
  printer_deadline_transport_cleanup(outer, cleanup, 0);
  assert(esp_transport_connect(outer, "192.168.1.2", 8883, 10000) == 0);
  memset(incoming, 'x', 4096);
  incoming[4096] = (char)0xd0; incoming[4097] = 0; /* Following MQTT PINGRESP. */
  char received[4096];
  size_t used = 0;
  while (used < sizeof(received)) {
    int size = esp_transport_read(outer, received + used, (int)(sizeof(received) - used),
                                 used < 2048 ? 0 : 10000);
    assert(size > 0); /* Incomplete TLS records must not truncate MQTT delivery. */
    used += (size_t)size;
  }
  assert(!memcmp(received, incoming, sizeof(received)));
  char next[2];
  assert(esp_transport_read(outer, next, sizeof(next), 0) == 2);
  assert((unsigned char)next[0] == 0xd0 && next[1] == 0);
  assert(socket_flags == 0x40);
  printer_deadline_transport_set(outer, 1000);
  assert(esp_transport_write(outer, "abc", 3, 10000) == 1);
  assert(now == 10 && socket_flags == 0x40);
  now = 990;
  assert(esp_transport_write(outer, "bc", 2, 10000) < 0 && writes == 1);
  now = 0;
  poll_mode = 1;
  assert(esp_transport_write(outer, "abc", 3, 10000) < 0 && now == 1000 &&
         writes == 1);
  now = 0;
  poll_mode = 2;
  assert(esp_transport_write(outer, "abc", 3, 10000) < 0 && now == 10 &&
         writes == 1);
  now = 0;
  permitted = true;
  slow_guard = true;
  assert(esp_transport_write(outer, "abc", 3, 10000) < 0 && writes == 1);
  slow_guard = false;
  now = 0;
  permitted = false;
  assert(esp_transport_write(outer, "abc", 3, 10000) < 0 && writes == 1);
  poll_mode = 0;
  permitted = true;
  fail_write = true;
  assert(esp_transport_write(outer, "abc", 3, 10000) < 0 && writes == 2);
  assert(socket_flags == 0x40);
  fail_write = false;
  fail_setup = true;
  assert(esp_transport_write(outer, "abc", 3, 10000) < 0 && writes == 2);
  fail_setup = false;
  setup_mode = 1;
  assert(esp_transport_write(outer, "abc", 3, 10000) < 0 && writes == 2);
  assert(socket_flags == 0x40);
  setup_mode = 2;
  permitted = true;
  now = 0;
  assert(esp_transport_write(outer, "abc", 3, 10000) < 0 && writes == 2);
  assert(socket_flags == 0x40);
  setup_mode = 0;
  now = 0;
  fail_restore = true;
  assert(esp_transport_write(outer, "abc", 3, 10000) < 0 && closes == 1);
  esp_transport_destroy(outer);
  assert(destroyed == 1);
  return 0;
}
