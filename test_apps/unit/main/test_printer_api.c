#include "printer_api_policy.h"
#include "unity.h"
#include <string.h>
TEST_CASE("printer API rejects duplicate arming fields", "[printer_api]") {
    printer_arm_request_t request;
    const char body[]="{\"arm_token\":\"0123456789abcdef0123456789abcdef\",\"initial_pause_confirmed\":true,\"initial_pause_confirmed\":true}";
    TEST_ASSERT_FALSE(printer_api_parse_arm(body,sizeof(body)-1,&request));
    printer_control_status_t status={.access_code_set=true};char json[2048];
    TEST_ESP_OK(printer_api_status_json(&status,"test",json,sizeof(json)));
    TEST_ASSERT_NULL(strstr(json,"\"access_code\":"));
}
