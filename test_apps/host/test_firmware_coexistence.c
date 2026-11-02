#include <assert.h>
#include <setjmp.h>
#include "../../components/firmware_update/firmware_update.c"
static jmp_buf finished;
static unsigned notifications,jobs,polls;
static bool suspended,blocked;
static uint64_t now;
const firmware_identity_t custom_app_desc={.version="1.0.0"};
int64_t esp_timer_get_time(void){return (int64_t)now*1000;}
void printer_link_suspend(bool value){suspended=value;}
bool printer_link_quiescent(void){return !blocked && polls>=2;}
void vTaskDelay(TickType_t ticks){assert(suspended);now+=(uint64_t)ticks*4;++polls;}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t ticks){(void)clear;(void)ticks;if(notifications++)longjmp(finished,1);return 1;}
BaseType_t xTaskNotifyGive(TaskHandle_t task){assert(task);return pdTRUE;}
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,uint32_t stack,void *arg,UBaseType_t priority,TaskHandle_t *out){(void)fn;(void)name;(void)stack;(void)arg;(void)priority;*out=(void *)1;return pdPASS;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void *)1;}
BaseType_t xSemaphoreTake(SemaphoreHandle_t lock,TickType_t ticks){(void)ticks;assert(lock);return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t lock){assert(lock);return pdTRUE;}
bool firmware_trial_pending(void){return false;}
bool app_runtime_maintenance_active(void){return false;}
esp_err_t app_runtime_begin_maintenance(uint32_t t){(void)t;assert(0);return ESP_FAIL;}
esp_err_t app_runtime_end_maintenance(void){assert(0);return ESP_FAIL;}
esp_err_t device_config_load_attempt(firmware_attempt_t *v){(void)v;assert(0);return ESP_FAIL;}
esp_err_t device_config_save_attempt(const firmware_attempt_t *v){(void)v;assert(0);return ESP_FAIL;}
esp_err_t firmware_transport_check(const char *v,firmware_candidate_t *c,firmware_check_result_t *r){(void)v;(void)c;(void)r;assert(0);return ESP_FAIL;}
esp_err_t firmware_transport_stage(const firmware_candidate_t *v,firmware_progress_cb_t cb,void *ctx,firmware_staged_t *out){(void)v;(void)cb;(void)ctx;(void)out;assert(0);return ESP_FAIL;}
esp_err_t firmware_transport_activate(const firmware_staged_t *v){(void)v;assert(0);return ESP_FAIL;}
void esp_restart(void){assert(0);}
void firmware_job_run(firmware_status_t *status,const firmware_job_ops_t *ops,firmware_status_cb_t cb,void *ctx){
    (void)ops;assert(suspended && printer_link_quiescent());++jobs;status->phase=FIRMWARE_FAILED;status->result=FIRMWARE_RESULT_CHECK_FAILED;cb(status,ctx);
}
static void run(void){notifications=polls=0;assert(firmware_update_request()==ESP_OK);if(!setjmp(finished))worker(NULL);assert(!suspended && service_operation_current()==SERVICE_OPERATION_NONE);}
int main(void){
    assert(firmware_update_start()==ESP_OK);
    assert(service_operation_begin(SERVICE_OPERATION_PRINTER_CYCLE));assert(firmware_update_request()==ESP_ERR_NOT_ALLOWED);service_operation_end(SERVICE_OPERATION_PRINTER_CYCLE);
    run();assert(jobs==1 && now>=200);
    blocked=true;run();assert(jobs==1 && now>=5200 && s_status.result==FIRMWARE_RESULT_CHECK_FAILED);return 0;
}
