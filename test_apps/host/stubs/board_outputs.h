#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct { int unused; } board_outputs_config_t;
esp_err_t board_outputs_init(const board_outputs_config_t *);
esp_err_t board_outputs_set(bool heater, bool fan);
