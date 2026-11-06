#include <stddef.h>

#include "board_outputs.h"
#include "driver/gpio.h"
#include "unity.h"

#define WRITE_LOG_CAPACITY 16U

typedef struct {
    int gpio[WRITE_LOG_CAPACITY];
    int level[WRITE_LOG_CAPACITY];
    size_t count;
} write_log_t;

static write_log_t s_log;

static void record_write(int gpio_num, int level, void *ctx)
{
    write_log_t *log = (write_log_t *) ctx;
    if (log->count < WRITE_LOG_CAPACITY) {
        log->gpio[log->count] = gpio_num;
        log->level[log->count] = level;
    }
    log->count++;
}

static void clear_log(void)
{
    s_log = (write_log_t) {0};
}

static esp_err_t init_fake(bool active_low)
{
    clear_log();
    const board_outputs_config_t config = {
        .active_low = active_low,
        .write = record_write,
        .write_ctx = &s_log,
    };
    return board_outputs_init(&config);
}

static void assert_write(size_t index, int gpio_num, int level)
{
    TEST_ASSERT_GREATER_THAN_UINT32((uint32_t) index, (uint32_t) s_log.count);
    TEST_ASSERT_EQUAL_INT(gpio_num, s_log.gpio[index]);
    TEST_ASSERT_EQUAL_INT(level, s_log.level[index]);
}

TEST_CASE("initialization drives both channels inactive first", "[board]")
{
    TEST_ESP_OK(init_fake(true));

    TEST_ASSERT_EQUAL_UINT32(2, s_log.count);
    assert_write(0, GPIO_NUM_16, 1);
    assert_write(1, GPIO_NUM_17, 1);
    TEST_ASSERT_EQUAL_INT(s_log.level[0], s_log.level[1]);
}

TEST_CASE("activation commands the fan before the heater", "[board]")
{
    TEST_ESP_OK(init_fake(true));
    clear_log();

    TEST_ESP_OK(board_outputs_set(true, true));

    TEST_ASSERT_EQUAL_UINT32(2, s_log.count);
    assert_write(0, GPIO_NUM_17, 0);
    assert_write(1, GPIO_NUM_16, 0);
    TEST_ASSERT_EQUAL_INT(s_log.level[0], s_log.level[1]);
}

TEST_CASE("deactivation commands the heater before the fan", "[board]")
{
    TEST_ESP_OK(init_fake(true));
    TEST_ESP_OK(board_outputs_set(true, true));
    clear_log();

    TEST_ESP_OK(board_outputs_set(false, false));

    TEST_ASSERT_EQUAL_UINT32(2, s_log.count);
    assert_write(0, GPIO_NUM_16, 1);
    assert_write(1, GPIO_NUM_17, 1);
    TEST_ASSERT_EQUAL_INT(s_log.level[0], s_log.level[1]);
}

TEST_CASE("the fan can remain on after the heater turns off", "[board]")
{
    TEST_ESP_OK(init_fake(true));
    TEST_ESP_OK(board_outputs_set(true, true));
    clear_log();

    TEST_ESP_OK(board_outputs_set(false, true));

    TEST_ASSERT_EQUAL_UINT32(1, s_log.count);
    assert_write(0, GPIO_NUM_16, 1);
}

TEST_CASE("the heater cannot run without the fan", "[board]")
{
    TEST_ESP_OK(init_fake(true));
    clear_log();

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_outputs_set(true, false));
    TEST_ASSERT_EQUAL_UINT32(0, s_log.count);
}

TEST_CASE("repeating a logical state does not toggle relays", "[board]")
{
    TEST_ESP_OK(init_fake(true));
    TEST_ESP_OK(board_outputs_set(true, true));
    clear_log();

    TEST_ESP_OK(board_outputs_set(true, true));
    TEST_ASSERT_EQUAL_UINT32(0, s_log.count);

    TEST_ESP_OK(board_outputs_set(false, false));
    TEST_ASSERT_EQUAL_UINT32(2, s_log.count);
    clear_log();

    TEST_ESP_OK(board_outputs_set(false, false));
    TEST_ASSERT_EQUAL_UINT32(0, s_log.count);
}

TEST_CASE("active-high boards invert the electrical mapping", "[board]")
{
    TEST_ESP_OK(init_fake(false));

    TEST_ASSERT_EQUAL_UINT32(2, s_log.count);
    assert_write(0, GPIO_NUM_16, 0);
    assert_write(1, GPIO_NUM_17, 0);
    clear_log();

    TEST_ESP_OK(board_outputs_set(true, true));
    TEST_ASSERT_EQUAL_UINT32(2, s_log.count);
    assert_write(0, GPIO_NUM_17, 1);
    assert_write(1, GPIO_NUM_16, 1);
}

TEST_CASE("forcing off rewrites both channels even when already off", "[board]")
{
    TEST_ESP_OK(init_fake(true));
    clear_log();

    TEST_ESP_OK(board_outputs_force_off());

    TEST_ASSERT_EQUAL_UINT32(2, s_log.count);
    assert_write(0, GPIO_NUM_16, 1);
    assert_write(1, GPIO_NUM_17, 1);
}

TEST_CASE("commands before initialization are rejected", "[board]")
{
    TEST_ESP_OK(init_fake(true));
    TEST_ESP_OK(board_outputs_deinit());
    clear_log();

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, board_outputs_set(true, true));
    TEST_ASSERT_EQUAL_UINT32(0, s_log.count);

    TEST_ESP_OK(init_fake(true));
}
