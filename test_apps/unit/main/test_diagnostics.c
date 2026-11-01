#include <stddef.h>

#include "diagnostics.h"
#include "unity.h"

TEST_CASE("the ring buffer keeps the newest thirty-two records", "[diagnostics]")
{
    diagnostics_reset();

    for (uint64_t index = 0U; index < 40U; ++index) {
        diagnostics_record(index, HEAT_STATE_IDLE, HEAT_STATE_HEATING,
                           HEAT_FAULT_NONE);
    }

    diagnostic_record_t records[DIAGNOSTICS_CAPACITY];
    size_t count = 0U;
    diagnostics_snapshot(records, DIAGNOSTICS_CAPACITY, &count);

    TEST_ASSERT_EQUAL_UINT32(DIAGNOSTICS_CAPACITY, count);
    /* Records 8 through 39 survive, oldest first. */
    for (size_t i = 0U; i < count; ++i) {
        TEST_ASSERT_EQUAL_UINT64((uint64_t) (i + 8U), records[i].timestamp_ms);
        TEST_ASSERT_EQUAL(HEAT_STATE_IDLE, records[i].from_state);
        TEST_ASSERT_EQUAL(HEAT_STATE_HEATING, records[i].to_state);
        TEST_ASSERT_EQUAL(HEAT_FAULT_NONE, records[i].fault);
    }
}

TEST_CASE("a partial buffer reports only what was recorded", "[diagnostics]")
{
    diagnostics_reset();

    diagnostics_record(10U, HEAT_STATE_BOOT, HEAT_STATE_IDLE, HEAT_FAULT_NONE);
    diagnostics_record(20U, HEAT_STATE_IDLE, HEAT_STATE_FAULT,
                       HEAT_FAULT_SENSOR_OPEN);

    diagnostic_record_t records[DIAGNOSTICS_CAPACITY];
    size_t count = 0U;
    diagnostics_snapshot(records, DIAGNOSTICS_CAPACITY, &count);

    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_UINT64(10U, records[0].timestamp_ms);
    TEST_ASSERT_EQUAL(HEAT_STATE_BOOT, records[0].from_state);
    TEST_ASSERT_EQUAL_UINT64(20U, records[1].timestamp_ms);
    TEST_ASSERT_EQUAL(HEAT_FAULT_SENSOR_OPEN, records[1].fault);
}

TEST_CASE("snapshots never write past the caller's array", "[diagnostics]")
{
    diagnostics_reset();

    for (uint64_t index = 0U; index < 10U; ++index) {
        diagnostics_record(index, HEAT_STATE_IDLE, HEAT_STATE_HOLDING,
                           HEAT_FAULT_NONE);
    }

    diagnostic_record_t records[4];
    size_t count = 0U;
    diagnostics_snapshot(records, 4U, &count);

    /* The newest four are the ones worth keeping when space is short. */
    TEST_ASSERT_EQUAL_UINT32(4, count);
    TEST_ASSERT_EQUAL_UINT64(6U, records[0].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT64(9U, records[3].timestamp_ms);

    diagnostics_snapshot(NULL, 4U, &count);
    TEST_ASSERT_EQUAL_UINT32(0, count);
}
