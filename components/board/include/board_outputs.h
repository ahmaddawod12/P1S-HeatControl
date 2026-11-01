#pragma once

#include <stdbool.h>

#include "board_pins.h"
#include "esp_err.h"

/* The fan may run independently for circulation and cooldown. The heater is
 * rejected unless the fan is also requested. When both channels change, the
 * fan leads on activation and the heater leads on deactivation. */
typedef void (*board_gpio_write_fn)(int gpio_num, int level, void *ctx);

typedef struct {
    bool active_low;
    board_gpio_write_fn write;
    void *write_ctx;
} board_outputs_config_t;

/* Production configuration: the build-time relay polarity and real GPIO writes.
 * A NULL `write` selects the hardware writer. */
board_outputs_config_t board_outputs_default_config(void);

/* Drives both channels inactive before returning. Passing NULL uses the
 * production configuration. */
esp_err_t board_outputs_init(const board_outputs_config_t *config);
esp_err_t board_outputs_set(bool heater_on, bool fan_on);
esp_err_t board_outputs_force_off(void);
esp_err_t board_outputs_deinit(void);
