#include "board_outputs.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static board_outputs_config_t s_config;
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static bool s_heater_on;
static bool s_fan_on;

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
static void write_pair(bool on)
{
    const int level = relay_level(on);
    if (on) {
        s_config.write(BOARD_FAN_GPIO, level, s_config.write_ctx);
        s_config.write(BOARD_HEATER_GPIO, level, s_config.write_ctx);
    } else {
        s_config.write(BOARD_HEATER_GPIO, level, s_config.write_ctx);
        s_config.write(BOARD_FAN_GPIO, level, s_config.write_ctx);
    }
}

/* Caller holds s_lock. Only writes channels whose logical state changes. */
static void write_outputs(bool heater_on, bool fan_on)
{
    if (fan_on && !s_fan_on) {
        s_config.write(BOARD_FAN_GPIO, relay_level(true), s_config.write_ctx);
    }
    if (!heater_on && s_heater_on) {
        s_config.write(BOARD_HEATER_GPIO, relay_level(false),
                       s_config.write_ctx);
    }
    if (heater_on && !s_heater_on) {
        s_config.write(BOARD_HEATER_GPIO, relay_level(true),
                       s_config.write_ctx);
    }
    if (!fan_on && s_fan_on) {
        s_config.write(BOARD_FAN_GPIO, relay_level(false), s_config.write_ctx);
    }
}

/* Order matters: the pull holds the pad inactive while it is still an input,
 * and the output register is preloaded with the inactive level before the
 * driver is enabled. Enabling the output first would drive the pad to its
 * reset value and pulse an active-low relay. */
static esp_err_t configure_pin(int gpio_num)
{
    const gpio_num_t pin = (gpio_num_t) gpio_num;
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

static esp_err_t configure_pins(void)
{
    const esp_err_t err = configure_pin(BOARD_HEATER_GPIO);
    return err == ESP_OK ? configure_pin(BOARD_FAN_GPIO) : err;
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
    esp_err_t err = hardware ? configure_pins() : ESP_OK;
    if (err == ESP_OK) {
        s_heater_on = false;
        s_fan_on = false;
        s_initialized = true;
        write_pair(false);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t board_outputs_set(bool heater_on, bool fan_on)
{
    if (heater_on && !fan_on) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_initialized) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_heater_on != heater_on || s_fan_on != fan_on) {
        write_outputs(heater_on, fan_on);
        s_heater_on = heater_on;
        s_fan_on = fan_on;
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
        write_pair(false);
        s_heater_on = false;
        s_fan_on = false;
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
        write_pair(false);
    }
    s_initialized = false;
    s_heater_on = false;
    s_fan_on = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
