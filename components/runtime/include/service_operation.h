#pragma once
#include <stdbool.h>
typedef enum {
    SERVICE_OPERATION_NONE, SERVICE_OPERATION_WIFI, SERVICE_OPERATION_OTA,
    SERVICE_OPERATION_TRIAL
} service_operation_t;
/* Only a successful admission may be ended. Owners span asynchronous work;
 * this gate never changes heater state. */
bool service_operation_begin(service_operation_t owner);
void service_operation_end(service_operation_t owner);
service_operation_t service_operation_current(void);
