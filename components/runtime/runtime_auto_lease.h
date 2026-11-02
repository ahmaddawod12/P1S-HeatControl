#pragma once
#include "esp_err.h"
#include "runtime_auto_types.h"
typedef struct {
  runtime_auto_status_t status;
  uint64_t highest_id;
} runtime_auto_lease_t;
void runtime_auto_lease_init(runtime_auto_lease_t *lease);
esp_err_t runtime_auto_lease_acquire(runtime_auto_lease_t *lease, uint64_t id,
                                     uint64_t now, uint64_t until);
esp_err_t runtime_auto_lease_renew(runtime_auto_lease_t *lease, uint64_t id,
                                   uint64_t now, uint64_t until);
void runtime_auto_lease_revoke(runtime_auto_lease_t *lease, uint64_t id,
                               runtime_auto_end_reason_t reason);
bool runtime_auto_lease_step(runtime_auto_lease_t *lease, uint64_t now);
