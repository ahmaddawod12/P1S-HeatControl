#include "clock_service_ops.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <windows.h>
static atomic_uint now, starts, stops, waits;
static atomic_bool valid_clock, cancel, slow;
static bool borrowed;
static uint64_t mono(void) { return atomic_load(&now); }
static bool valid(void) { return atomic_load(&valid_clock); }
static esp_err_t start(const char *host) {
  assert(!strcmp(host, "lan.example"));
  atomic_fetch_add(&starts, 1);
  return borrowed ? ESP_ERR_INVALID_STATE : ESP_OK;
}
static void wait_clock(uint32_t ms) {
  assert(ms <= 100);
  atomic_fetch_add(&now, ms);
  unsigned n = atomic_fetch_add(&waits, 1) + 1;
  if (atomic_load(&slow))
    Sleep(5);
  if (n >= 3)
    atomic_store(&valid_clock, true);
}
static void stop(void) { atomic_fetch_add(&stops, 1); }
static void sleep_clock(uint32_t ms) {
  assert(ms <= 100);
  Sleep(1);
}
static bool cancelled(void *p) {
  (void)p;
  return atomic_load(&cancel) || mono() >= 100;
}
static const clock_service_ops_t ops = {mono,       valid, start,
                                        wait_clock, stop,  sleep_clock};
static void reset(void) {
  now = starts = stops = waits = 0;
  valid_clock = cancel = slow = false;
  borrowed = false;
}
static void test_shared_clock_cancellation(void) {
  reset();
  assert(clock_service_wait_with_ops(&ops, "lan.example", 30000, cancelled,
                                     NULL) == ESP_ERR_TIMEOUT);
  assert(now == 100 && starts == 1 && stops == 1);
  reset();
  borrowed = true;
  assert(clock_service_wait_with_ops(&ops, "lan.example", 30000, NULL, NULL) ==
         ESP_OK);
  assert(starts == 1 && stops == 0);
  /* A valid wall clock does not initialize a second SNTP client. */
  assert(clock_service_wait_with_ops(&ops, "lan.example", 30000, NULL, NULL) ==
         ESP_OK);
  assert(starts == 1);
}
static void *caller(void *p) {
  (void)p;
  assert(clock_service_wait_with_ops(&ops, "lan.example", 30000, NULL, NULL) ==
         ESP_OK);
  return NULL;
}
int main(void) {
  test_shared_clock_cancellation();
  reset();
  slow = true;
  pthread_t a, b;
  assert(!pthread_create(&a, NULL, caller, NULL));
  assert(!pthread_create(&b, NULL, caller, NULL));
  pthread_join(a, NULL);
  pthread_join(b, NULL);
  assert(starts == 1 && stops == 1);
  return 0;
}
