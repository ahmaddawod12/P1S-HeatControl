#include "runtime_auto_lease.h"
#include <string.h>
void runtime_auto_lease_init(runtime_auto_lease_t *p) {
  memset(p, 0, sizeof(*p));
  p->status.teardown_applied = true;
}
void runtime_auto_lease_revoke(runtime_auto_lease_t *p, uint64_t id,
                               runtime_auto_end_reason_t reason) {
  if (p->status.active && (!id || id == p->status.cycle_id)) {
    p->status.active = false;
    p->status.last_ended_id = p->status.cycle_id;
    p->status.reason = reason;
    p->status.teardown_applied = false;
  }
}
bool runtime_auto_lease_step(runtime_auto_lease_t *p, uint64_t now) {
  if (p->status.active && now >= p->status.expires_ms)
    runtime_auto_lease_revoke(p, 0, RUNTIME_AUTO_EXPIRED);
  return p->status.active;
}
esp_err_t runtime_auto_lease_acquire(runtime_auto_lease_t *p, uint64_t id,
                                     uint64_t now, uint64_t until) {
  if (!id || until <= now || until - now > 5000)
    return ESP_ERR_INVALID_ARG;
  (void)runtime_auto_lease_step(p, now);
  if (p->status.active)
    return ESP_ERR_NOT_ALLOWED;
  if (id <= p->highest_id)
    return ESP_ERR_INVALID_STATE;
  p->highest_id = id;
  p->status = (runtime_auto_status_t){
      .active = true, .cycle_id = id, .expires_ms = until};
  return ESP_OK;
}
esp_err_t runtime_auto_lease_renew(runtime_auto_lease_t *p, uint64_t id,
                                   uint64_t now, uint64_t until) {
  if (!runtime_auto_lease_step(p, now) || id != p->status.cycle_id)
    return ESP_ERR_INVALID_STATE;
  if (until <= now || until - now > 5000)
    return ESP_ERR_INVALID_ARG;
  p->status.expires_ms = until;
  return ESP_OK;
}
