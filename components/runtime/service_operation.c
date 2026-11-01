#include "service_operation.h"
#include <stdatomic.h>

static atomic_int s_owner;

bool service_operation_begin(service_operation_t owner)
{
    if (owner <= SERVICE_OPERATION_NONE || owner > SERVICE_OPERATION_TRIAL) return false;
    int expected = SERVICE_OPERATION_NONE;
    return atomic_compare_exchange_strong(&s_owner, &expected, (int)owner);
}
void service_operation_end(service_operation_t owner)
{
    if (owner <= SERVICE_OPERATION_NONE || owner > SERVICE_OPERATION_TRIAL) return;
    int expected = (int)owner;
    (void)atomic_compare_exchange_strong(&s_owner, &expected, SERVICE_OPERATION_NONE);
}
service_operation_t service_operation_current(void)
{ return (service_operation_t)atomic_load(&s_owner); }
