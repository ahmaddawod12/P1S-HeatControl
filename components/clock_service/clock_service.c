#include "clock_service_ops.h"
#include <stdatomic.h>
/* SNTP is global. Waiters poll the owner rather than initializing another
 * client. */
static atomic_flag owner = ATOMIC_FLAG_INIT;
esp_err_t clock_service_wait_with_ops(const clock_service_ops_t *ops,
                                      const char *host, uint32_t timeout,
                                      bool (*cancelled)(void *), void *ctx) {
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
  if (ops->valid()) {
    result = ESP_OK;
    goto done;
  }
  result = ops->start(host);
  started = result == ESP_OK;
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    goto done;
  result = ESP_ERR_TIMEOUT;
  while (ops->monotonic_ms() < deadline && !(cancelled && cancelled(ctx))) {
    if (ops->valid()) {
      result = ESP_OK;
      break;
    }
    uint64_t left = deadline - ops->monotonic_ms();
    ops->wait(left > 100 ? 100 : (uint32_t)left);
  }
  if (ops->valid() && !(cancelled && cancelled(ctx)))
    result = ESP_OK;
done:
  if (started)
    ops->stop();
  if (acquired)
    atomic_flag_clear(&owner);
  return result;
}
