#include "clock_service_ops.h"
#include <stddef.h>
#include <stdatomic.h>
/* SNTP is global. Waiters poll the owner rather than initializing another
 * client. */
static atomic_flag owner = ATOMIC_FLAG_INIT;
/* Store monotonic seconds plus one so zero means no sync yet. */
static atomic_uint_least32_t last_sync;

static uint32_t monotonic_seconds(const clock_service_ops_t *ops) {
  return (uint32_t)(ops->monotonic_ms() / 1000U) + 1U;
}

bool clock_service_is_recent_with_ops(const clock_service_ops_t *ops,
                                      uint32_t max_age_seconds) {
  const uint32_t synced_at = atomic_load(&last_sync);
  return synced_at && (uint32_t)(monotonic_seconds(ops) - synced_at) < max_age_seconds &&
         ops->valid();
}

static esp_err_t wait_for_clock(const clock_service_ops_t *ops,
                                const char *host, uint32_t timeout,
                                bool (*cancelled)(void *), void *ctx,
                                bool require_sync) {
  if (!ops || !host || !host[0] || !timeout)
    return ESP_ERR_INVALID_ARG;
  uint64_t deadline = ops->monotonic_ms() + timeout;
  bool acquired = false;
  while (!(acquired = !atomic_flag_test_and_set(&owner))) {
    if ((cancelled && cancelled(ctx)) || ops->monotonic_ms() >= deadline)
      return ESP_ERR_TIMEOUT;
    ops->sleep(100);
  }
  esp_err_t result = ESP_ERR_TIMEOUT;
  bool started = false;
  if ((cancelled && cancelled(ctx)) || ops->monotonic_ms() >= deadline)
    goto done;
  if (!require_sync && ops->valid()) {
    result = ESP_OK;
    goto done;
  }
  result = ops->start(host);
  started = result == ESP_OK;
  if (!started && (require_sync || result != ESP_ERR_INVALID_STATE))
    goto done;

  result = ESP_ERR_TIMEOUT;
  while (!(cancelled && cancelled(ctx))) {
    const uint64_t now = ops->monotonic_ms();
    if (now >= deadline || (!require_sync && ops->valid())) break;
    const uint64_t left = deadline - now;
    if (ops->wait(left > 100 ? 100 : (uint32_t)left) && ops->valid() && started) {
      atomic_store(&last_sync, monotonic_seconds(ops));
      if (!(cancelled && cancelled(ctx))) result = ESP_OK;
      break;
    }
  }
  if (!require_sync && ops->valid() && !(cancelled && cancelled(ctx)))
    result = ESP_OK;
done:
  if (started)
    ops->stop();
  if (acquired)
    atomic_flag_clear(&owner);
  return result;
}

esp_err_t clock_service_wait_with_ops(const clock_service_ops_t *ops,
                                      const char *host, uint32_t timeout,
                                      bool (*cancelled)(void *), void *ctx) {
  return wait_for_clock(ops, host, timeout, cancelled, ctx, false);
}

esp_err_t clock_service_sync_with_ops(const clock_service_ops_t *ops,
                                      const char *host, uint32_t timeout) {
  return wait_for_clock(ops, host, timeout, NULL, NULL, true);
}
