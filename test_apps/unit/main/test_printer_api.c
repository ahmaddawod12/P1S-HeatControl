#include "printer_api_policy.h"
#include "unity.h"
#include <string.h>
TEST_CASE("printer API status reports monitoring, never the access code", "[printer_api]") {
    printer_control_status_t status={.access_code_set=true};
    status.monitor.phase=PRINTER_MONITOR_WATCHING;
    char json[2048];
    TEST_ESP_OK(printer_api_status_json(&status,"test",json,sizeof(json)));
    TEST_ASSERT_NULL(strstr(json,"\"access_code\":"));
    TEST_ASSERT_NOT_NULL(strstr(json,"\"phase\":\"watching\""));
    TEST_ASSERT_NULL(strstr(json,"arm_token"));
}
