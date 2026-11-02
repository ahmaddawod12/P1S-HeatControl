#include "app_startup.h"
#include "web_server.h"
#include "board_outputs.h"
#include <assert.h>
static unsigned sequence,force_off;
static esp_err_t step(unsigned expected){assert(sequence++==expected);return ESP_OK;}
esp_err_t board_outputs_init(const board_outputs_config_t *c){(void)c;return step(0);}
esp_err_t firmware_trial_start(void){return step(1);}
void firmware_trial_startup_progress(void){}
esp_err_t nvs_flash_init(void){return step(2);}
esp_err_t nvs_flash_erase(void){assert(0);return ESP_FAIL;}
esp_err_t settings_init(void){return step(3);}
esp_err_t app_runtime_start(void){return step(4);}
esp_err_t wifi_manager_start(void){return step(5);}
web_server_config_t web_server_default_config(void){return (web_server_config_t){0};}
esp_err_t web_server_start(const web_server_config_t *c){(void)c;return step(6);}
esp_err_t board_outputs_force_off(void){++force_off;return ESP_OK;}
esp_err_t app_runtime_report_startup_fault(heat_fault_t f){(void)f;return ESP_OK;}
esp_err_t printer_control_start(void){step(7);return ESP_ERR_NO_MEM;}
void vTaskDelay(unsigned ticks){(void)ticks;assert(0);}
static void test_optional_printer_failure_is_nonfatal(void){
    app_startup_ops_t ops=app_startup_default_ops();assert(ops.printer_start==printer_control_start);
    assert(app_startup_run(&ops)==ESP_OK);assert(sequence==8 && !force_off);
    sequence=0;ops.printer_start=0;assert(app_startup_run(&ops)==ESP_OK);assert(sequence==7 && !force_off);
}
int main(void){test_optional_printer_failure_is_nonfatal();return 0;}
