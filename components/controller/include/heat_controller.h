#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

typedef enum {
    HEAT_COMMAND_ENABLE = 0,
    HEAT_COMMAND_DISABLE,
    HEAT_COMMAND_SET_TARGET,
    HEAT_COMMAND_ACK_FAULT,
    HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
} heat_controller_command_kind_t;

typedef struct {
    heat_controller_command_kind_t kind;
    float target_c;
    heat_fault_t fault;
} heat_controller_command_t;

typedef struct {
    float target_c;
    float hysteresis_c;
    float overtemp_c;
    uint32_t min_transition_ms;
} heat_controller_config_t;

typedef struct {
    heat_state_t state;
    heat_fault_t fault;
    bool enabled;
    bool output_on;
    float target_c;
} heat_controller_snapshot_t;

typedef struct {
    heat_controller_config_t config;
    heat_controller_snapshot_t snapshot;
    uint64_t last_transition_ms;
    bool has_transition;
} heat_controller_t;

bool heat_target_is_valid(float target_c);
void heat_controller_init(heat_controller_t *controller,
                          const heat_controller_config_t *config);
bool heat_controller_command(heat_controller_t *controller,
                             const heat_controller_command_t *command,
                             const temperature_sample_t *sample,
                             uint64_t now_ms);
void heat_controller_step(heat_controller_t *controller,
                          const temperature_sample_t *sample,
                          uint64_t now_ms);
heat_controller_snapshot_t heat_controller_snapshot(
    const heat_controller_t *controller);
