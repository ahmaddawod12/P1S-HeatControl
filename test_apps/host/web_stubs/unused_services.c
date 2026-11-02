/* Link-only boundaries for web_server.c's default wiring and embedded assets.
 * These are never used by the handler/broadcast tests. */
#include <stdlib.h>
#include "web_server.h"
#include "app_runtime.h"
#include "printer_control.h"
#include "esp_http_server.h"
#include "freertos/task.h"
#include "diagnostics.h"
#include "sse_hub.h"
#include "wifi_api.h"
#include "printer_api.h"
#define UNUSED() abort()
void app_runtime_get_status(app_status_t *s) {(void)s;UNUSED();}
esp_err_t app_runtime_set_target(float v) {(void)v;UNUSED();}
esp_err_t app_runtime_set_enabled(bool v) {(void)v;UNUSED();}
esp_err_t app_runtime_set_manual_fan(bool v) {(void)v;UNUSED();}
esp_err_t app_runtime_ack_fault(void) {UNUSED();}
void firmware_update_get_status(firmware_status_t *s) {(void)s;UNUSED();}
esp_err_t firmware_update_request(void) {UNUSED();}
void wifi_manager_get_status(wifi_manager_status_t *s) {(void)s;UNUSED();}
esp_err_t wifi_manager_request_credentials(const wifi_credentials_t *s) {(void)s;UNUSED();}
void printer_control_get_status(printer_control_status_t *s) {(void)s;UNUSED();}
esp_err_t printer_control_request_config(const printer_config_request_t *s) {(void)s;UNUSED();}
esp_err_t printer_control_forget_config(void) {UNUSED();}
esp_err_t printer_control_arm(const printer_arm_request_t *s) {(void)s;UNUSED();}
esp_err_t printer_control_stop(void) {UNUSED();}
void diagnostics_snapshot(diagnostic_record_t *r,size_t cap,size_t *count) {(void)r;(void)cap;(void)count;UNUSED();}
esp_err_t sse_hub_add_client(httpd_req_t *r) {(void)r;UNUSED();}
void sse_hub_close_all(void) {UNUSED();}
esp_err_t httpd_start(httpd_handle_t *h,const httpd_config_t *c) {(void)h;(void)c;UNUSED();}
esp_err_t httpd_stop(httpd_handle_t h) {(void)h;UNUSED();}
esp_err_t wifi_api_register(httpd_handle_t h,const web_server_ops_t *s) {(void)h;(void)s;UNUSED();}
esp_err_t printer_api_register(httpd_handle_t h,const web_server_ops_t *s) {(void)h;(void)s;UNUSED();}
BaseType_t xTaskCreate(void (*f)(void *),const char *n,uint32_t stack,void *arg,UBaseType_t priority,TaskHandle_t *task) {
    (void)f;(void)n;(void)stack;(void)arg;(void)priority;(void)task;UNUSED();
}
#define ASSET(name) const char name##_start[] asm("_binary_" #name "_start")=""; const char name##_end[] asm("_binary_" #name "_end")=""
ASSET(index_html);ASSET(styles_css);ASSET(app_js);ASSET(wifi_js);ASSET(printer_js);
