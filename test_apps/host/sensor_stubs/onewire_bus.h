#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct fake_onewire_bus *onewire_bus_handle_t;
typedef struct {
    int bus_gpio_num;
    struct { unsigned en_pull_up : 1; } flags;
} onewire_bus_config_t;
typedef struct { unsigned max_rx_bytes; } onewire_bus_rmt_config_t;

esp_err_t onewire_new_bus_rmt(const onewire_bus_config_t *,
                             const onewire_bus_rmt_config_t *,
                             onewire_bus_handle_t *);
esp_err_t onewire_bus_del(onewire_bus_handle_t);
