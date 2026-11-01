#include "unity.h"

#include "app_types.h"

TEST_CASE("status starts disabled with relays off", "[app_types]")
{
    app_status_t status = app_status_initial();

    TEST_ASSERT_EQUAL(HEAT_STATE_BOOT, status.state);
    TEST_ASSERT_EQUAL(HEAT_FAULT_NONE, status.fault);
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_FALSE(status.fan_on);
    TEST_ASSERT_FALSE(status.manual_fan_on);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, status.target_c);
}
