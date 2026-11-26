#include "app_startup.h"
#include "firebase_client.h"
#include "board_outputs.h"
#include <assert.h>
#include <limits.h>

static unsigned sequence, force_off, updater_starts, startup_reports;
static unsigned failing_step = UINT_MAX;
static esp_err_t startup_result;

void app_main(void);

static esp_err_t step(unsigned expected)
{
    assert(sequence++ == expected);
    return expected == failing_step ? ESP_FAIL : ESP_OK;
}
esp_err_t board_outputs_init(const board_outputs_config_t *c){(void)c;return step(0);}
esp_err_t firmware_trial_start(void){return step(1);}
void firmware_trial_startup_progress(void){}
void firmware_trial_startup_complete(esp_err_t result)
{
    ++startup_reports;
    startup_result = result;
}
esp_err_t firmware_update_start(void) { ++updater_starts; return ESP_OK; }
esp_err_t nvs_flash_init(void){return step(2);}
esp_err_t nvs_flash_erase(void){assert(0);return ESP_FAIL;}
esp_err_t settings_init(void){return step(3);}
esp_err_t app_runtime_start(void){return step(4);}
esp_err_t wifi_manager_start(void){return step(5);}
esp_err_t firebase_client_start(void){return step(6);}
esp_err_t board_outputs_force_off(void){++force_off;return ESP_OK;}
esp_err_t app_runtime_report_startup_fault(heat_fault_t f){(void)f;return ESP_OK;}
esp_err_t printer_control_start(void){step(7);return ESP_ERR_NO_MEM;}
void vTaskDelay(unsigned ticks){(void)ticks;assert(0);}
static void test_optional_printer_failure_is_nonfatal(void){
    app_startup_ops_t ops=app_startup_default_ops();assert(ops.printer_start==printer_control_start);
    assert(ops.firebase_start==firebase_client_start);
    bool ready;
    assert(app_startup_run(&ops, &ready)==ESP_OK);assert(sequence==8 && !force_off && ready);
    sequence=0;ops.printer_start=0;assert(app_startup_run(&ops, &ready)==ESP_OK);assert(sequence==7 && !force_off && ready);
}

static void test_fatal_startup_stays_off(unsigned failed_step)
{
    failing_step = failed_step;
    sequence = force_off = updater_starts = startup_reports = 0;
    app_main();
    assert(sequence == failed_step + 1);
    assert(force_off == 1 && updater_starts == 0 && startup_reports == 1);
    assert(startup_result == ESP_FAIL);
}

int main(void)
{
    test_optional_printer_failure_is_nonfatal();
    failing_step = 5;
    sequence = 0;
    const app_startup_ops_t ops = app_startup_default_ops();
    bool ready;
    assert(app_startup_run(&ops, &ready) == ESP_FAIL && ready);
    test_fatal_startup_stays_off(0);
    test_fatal_startup_stays_off(4);
    return 0;
}
