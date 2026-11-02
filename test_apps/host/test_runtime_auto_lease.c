#include "runtime_auto_lease.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  runtime_auto_lease_t p;
  runtime_auto_lease_init(&p);
  assert(runtime_auto_lease_acquire(&p, 0, 1000, 6000) == ESP_ERR_INVALID_ARG);
  assert(runtime_auto_lease_acquire(&p, 1, 1000, 6001) == ESP_ERR_INVALID_ARG);
  assert(runtime_auto_lease_acquire(&p, 1, 1000, 6000) == ESP_OK);
  assert(runtime_auto_lease_step(&p, 5999));
  assert(!runtime_auto_lease_step(&p, 6000));
  assert(p.status.last_ended_id == 1 &&
         p.status.reason == RUNTIME_AUTO_EXPIRED);
  assert(runtime_auto_lease_renew(&p, 1, 6000, 11000) == ESP_ERR_INVALID_STATE);
  assert(runtime_auto_lease_acquire(&p, 1, 6000, 11000) ==
         ESP_ERR_INVALID_STATE);
  assert(runtime_auto_lease_acquire(&p, 2, 6000, 11000) == ESP_OK);
  runtime_auto_lease_revoke(&p, 1, RUNTIME_AUTO_OPERATOR_STOP);
  assert(p.status.active);
  runtime_auto_lease_revoke(&p, 0, RUNTIME_AUTO_OPERATOR_STOP);
  assert(!p.status.active);
  assert(runtime_auto_lease_acquire(&p, 1, 7000, 12000) ==
         ESP_ERR_INVALID_STATE);
  assert(runtime_auto_lease_acquire(&p, 3, 7000, 12000) == ESP_OK);
  assert(runtime_auto_lease_renew(&p, 3, 8000, 13000) == ESP_OK);
  assert(runtime_auto_lease_step(&p, 12000));
  assert(!runtime_auto_lease_step(&p, 13000));
  puts("runtime automatic lease passed");
}
