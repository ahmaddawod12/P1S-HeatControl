#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_types.h"

int main(void)
{
    const app_status_t status = app_status_initial();

    assert(status.state == HEAT_STATE_BOOT);
    assert(status.fault == HEAT_FAULT_NONE);
    assert(!status.enabled);
    assert(!status.heater_on);
    assert(!status.fan_on);
    assert(fabsf(status.target_c - 60.0f) < 0.001f);
    assert(strcmp(status.ip_addr, "0.0.0.0") == 0);

    puts("app_types_initial_status: PASS");
    return 0;
}
