#include "service_operation.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_bool start;
static atomic_int admitted;
static void *contend(void *arg)
{
    while (!atomic_load(&start)) sched_yield();
    if (service_operation_begin(*(service_operation_t *)arg)) atomic_fetch_add(&admitted, 1);
    return NULL;
}
int main(void)
{
    assert(service_operation_current() == SERVICE_OPERATION_NONE);
    assert(!service_operation_begin(SERVICE_OPERATION_NONE));
    assert(service_operation_begin(SERVICE_OPERATION_TRIAL));
    assert(service_operation_current() == SERVICE_OPERATION_TRIAL);
    assert(!service_operation_begin(SERVICE_OPERATION_WIFI));
    assert(!service_operation_begin(SERVICE_OPERATION_OTA));
    service_operation_end(SERVICE_OPERATION_WIFI);
    assert(service_operation_current() == SERVICE_OPERATION_TRIAL);
    service_operation_end(SERVICE_OPERATION_TRIAL);
    for(service_operation_t a=SERVICE_OPERATION_WIFI;a<=SERVICE_OPERATION_PRINTER_CYCLE;a++){
        assert(service_operation_begin(a));
        for(service_operation_t b=SERVICE_OPERATION_WIFI;b<=SERVICE_OPERATION_PRINTER_CYCLE;b++)assert(!service_operation_begin(b));
        service_operation_end(a);
    }
    for (unsigned i=0; i<32; ++i) {
        pthread_t a,b;
        service_operation_t wifi=SERVICE_OPERATION_WIFI, ota=SERVICE_OPERATION_OTA;
        atomic_store(&start,false); atomic_store(&admitted,0);
        assert(pthread_create(&a,NULL,contend,&wifi)==0);
        assert(pthread_create(&b,NULL,contend,&ota)==0);
        atomic_store(&start,true);
        pthread_join(a,NULL); pthread_join(b,NULL);
        assert(atomic_load(&admitted)==1);
        service_operation_end(service_operation_current());
    }
    puts("service operation admission passed");
    return 0;
}
