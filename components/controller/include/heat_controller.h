#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

#define HEAT_DEFAULT_HYSTERESIS_C 1.0f
#define HEAT_MIN_HYSTERESIS_C 0.5f
#define HEAT_MAX_HYSTERESIS_C 20.0f

typedef enum {
    HEAT_COMMAND_ENABLE = 0,
    HEAT_COMMAND_DISABLE,
    HEAT_COMMAND_SET_TARGET,
    HEAT_COMMAND_ACK_FAULT,
    HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
    HEAT_COMMAND_SET_RESTART,
} heat_controller_command_kind_t;

typedef struct {
    heat_controller_command_kind_t kind;
    float target_c;
    heat_fault_t fault;
    float restart_c;
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
    bool restart_lockout;
    /* Printer monitoring holds the element off while heating stays enabled. */
    bool inhibited;
    float target_c;
    float restart_c;
} heat_controller_snapshot_t;

typedef struct {
    heat_controller_config_t config;
    heat_controller_snapshot_t snapshot;
    uint64_t relay_off_at_ms;
    bool relay_off_recorded;
    bool manual_start_pending;
} heat_controller_t;

bool heat_target_is_valid(float target_c);
bool heat_hysteresis_is_valid(float hysteresis_c);
bool heat_restart_is_valid(float target_c, float restart_c);
void heat_controller_init(heat_controller_t *controller,
                          const heat_controller_config_t *config);
/* Start the minimum off interval at a confirmed physical shutoff. */
void heat_controller_note_relay_off(heat_controller_t *controller, uint64_t now_ms);
bool heat_controller_command(heat_controller_t *controller,
                             const heat_controller_command_t *command,
                             const temperature_sample_t *sample,
                             uint64_t now_ms);
/* While inhibited an enabled controller keeps its element off (HOLDING).
 * Releasing waits for the next step and the normal minimum transition time. */
void heat_controller_set_inhibit(heat_controller_t *controller, bool inhibited);
void heat_controller_step(heat_controller_t *controller,
                          const temperature_sample_t *sample,
                          uint64_t now_ms);
heat_controller_snapshot_t heat_controller_snapshot(
    const heat_controller_t *controller);
