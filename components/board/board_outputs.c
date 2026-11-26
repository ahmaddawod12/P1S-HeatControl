#include "board_outputs.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static board_outputs_config_t s_config;
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static bool s_on;

static void hardware_write(int gpio_num, int level, void *ctx)
{
    (void) ctx;
    (void) gpio_set_level((gpio_num_t) gpio_num, level);
}

static int relay_level(bool on)
{
    return s_config.active_low ? !on : on;
}

/* Caller holds s_lock. */
static void write_relay(bool on)
{
    s_config.write(BOARD_RELAY_GPIO, relay_level(on), s_config.write_ctx);
    s_on = on;
}

/* Order matters: the pull holds the pad inactive while it is still an input,
 * and the output register is preloaded with the inactive level before the
 * driver is enabled. Enabling the output first would drive the pad to its
 * reset value and pulse an active-low relay. */
static esp_err_t configure_pin(void)
{
    const gpio_num_t pin = BOARD_RELAY_GPIO;
    esp_err_t err = gpio_reset_pin(pin);
    if (err == ESP_OK) {
        err = gpio_set_pull_mode(pin, s_config.active_low ? GPIO_PULLUP_ONLY
                                                          : GPIO_PULLDOWN_ONLY);
    }
    if (err == ESP_OK) {
        err = gpio_set_level(pin, relay_level(false));
    }
    if (err == ESP_OK) {
        err = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    }
    return err;
}

board_outputs_config_t board_outputs_default_config(void)
{
    return (board_outputs_config_t) {
#if CONFIG_P1S_RELAY_ACTIVE_LOW
        .active_low = true,
#else
        .active_low = false,
#endif
        .write = NULL,
        .write_ctx = NULL,
    };
}

esp_err_t board_outputs_init(const board_outputs_config_t *config)
{
    board_outputs_config_t effective =
        config != NULL ? *config : board_outputs_default_config();
    const bool hardware = effective.write == NULL;
    if (hardware) {
        effective.write = hardware_write;
        effective.write_ctx = NULL;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = effective;
    const esp_err_t err = hardware ? configure_pin() : ESP_OK;
    s_initialized = err == ESP_OK;
    if (s_initialized) write_relay(false);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t board_outputs_set(bool on)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_initialized) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_on != on) {
        write_relay(on);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t board_outputs_force_off(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_initialized) {
        write_relay(false);
    } else {
        err = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t board_outputs_deinit(void)
{
    if (s_lock == NULL) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_initialized) {
        write_relay(false);
    }
    s_initialized = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
