#include "printer_deadline_transport.h"
#include <assert.h>
static uint64_t now;
static bool permitted = true;
static unsigned writes;
static unsigned destroyed;
static unsigned poll_mode;
static bool slow_guard;
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
  ++writes;
  return 1;
}
int main(void) {
  esp_transport_handle_t inner = esp_transport_init();
  esp_transport_set_func(inner, 0, 0, partial_write, 0, 0, poll, 0);
  esp_transport_handle_t outer =
      printer_deadline_transport_create(inner, clock_now, allowed, 0);
  assert(outer);
  printer_deadline_transport_cleanup(outer, cleanup, 0);
  assert(esp_transport_connect(outer, "192.168.1.2", 8883, 10000) == 0);
  printer_deadline_transport_set(outer, 1000);
  assert(esp_transport_write(outer, "abc", 3, 10000) == 1);
  assert(now == 10);
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
  esp_transport_destroy(outer);
  assert(destroyed == 1);
  return 0;
}
