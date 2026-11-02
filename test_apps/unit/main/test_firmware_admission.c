#include "firmware_update.h"
#include "service_operation.h"
#include "unity.h"

TEST_CASE("firmware admission rejects Wi-Fi and trial operation owners", "[firmware]")
{
    TEST_ESP_OK(firmware_update_start());
    TEST_ASSERT_TRUE(service_operation_begin(SERVICE_OPERATION_WIFI));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED,firmware_update_request());
    TEST_ASSERT_EQUAL(SERVICE_OPERATION_WIFI,service_operation_current());
    service_operation_end(SERVICE_OPERATION_WIFI);
    TEST_ASSERT_TRUE(service_operation_begin(SERVICE_OPERATION_TRIAL));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED,firmware_update_request());
    service_operation_end(SERVICE_OPERATION_TRIAL);
    TEST_ASSERT_TRUE(service_operation_begin(SERVICE_OPERATION_PRINTER_CYCLE));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED,firmware_update_request());
    service_operation_end(SERVICE_OPERATION_PRINTER_CYCLE);
    TEST_ASSERT_TRUE(service_operation_begin(SERVICE_OPERATION_PRINTER_CONFIG));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED,firmware_update_request());
    service_operation_end(SERVICE_OPERATION_PRINTER_CONFIG);
    TEST_ASSERT_EQUAL(SERVICE_OPERATION_NONE,service_operation_current());
}
