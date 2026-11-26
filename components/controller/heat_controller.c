#include "heat_controller.h"

#include <math.h>
#include <string.h>

static bool sample_has_overtemperature(const heat_controller_t *controller,
                                       const temperature_sample_t *sample)
{
    return sample != NULL &&
           ((isfinite(sample->probe_c) &&
             sample->probe_c >= controller->config.overtemp_c) ||
            (isfinite(sample->raw_c) &&
             sample->raw_c >= controller->config.overtemp_c) ||
            (isfinite(sample->filtered_c) &&
             sample->filtered_c >= controller->config.overtemp_c));
}

static bool sample_is_healthy(const temperature_sample_t *sample)
{
    return sample != NULL && sample->valid &&
           sample->fault == HEAT_FAULT_NONE &&
           isfinite(sample->probe_c) && isfinite(sample->raw_c) && isfinite(sample->filtered_c);
}

static bool sample_is_retrying(const temperature_sample_t *sample)
{
    if (sample->consecutive_failures == 0U ||
        sample->consecutive_failures >= TEMPERATURE_SENSOR_FAILURE_LIMIT) {
        return false;
    }
    return sample->fault == HEAT_FAULT_SENSOR_OPEN ||
           sample->fault == HEAT_FAULT_SENSOR_SHORT ||
           sample->fault == HEAT_FAULT_SENSOR_RANGE ||
           sample->fault == HEAT_FAULT_SENSOR_INVALID;
}

static void force_idle(heat_controller_t *controller)
{
    controller->snapshot.output_on = false;
    controller->manual_start_pending = false;
    controller->snapshot.enabled = false;
    controller->snapshot.state = controller->snapshot.fault == HEAT_FAULT_NONE
        ? HEAT_STATE_IDLE : HEAT_STATE_FAULT;
}

static void hold_output(heat_controller_t *controller)
{
    controller->snapshot.output_on = false;
    controller->manual_start_pending = false;
    controller->snapshot.state = HEAT_STATE_HOLDING;
}

static void latch_fault(heat_controller_t *controller, heat_fault_t fault)
{
    /* A startup storage fault can change the reported fault while the
     * controller still needs to acknowledge an interrupted heating session. */
    if (fault == HEAT_FAULT_RESTART_LOCKOUT) {
        controller->snapshot.restart_lockout = true;
    }
    /* Relay initialization failures require a restart. Later recoverable
     * faults must not replace them and make acknowledgement possible. */
    if (controller->snapshot.fault != HEAT_FAULT_OUTPUT_INIT) {
        controller->snapshot.fault = fault == HEAT_FAULT_NONE
            ? HEAT_FAULT_SENSOR_INVALID
            : fault;
    }
    force_idle(controller);
}

static bool off_interval_elapsed(const heat_controller_t *controller, uint64_t now_ms)
{
    return !controller->relay_off_recorded ||
           (now_ms >= controller->relay_off_at_ms &&
            now_ms - controller->relay_off_at_ms >= controller->config.min_transition_ms);
}

bool heat_target_is_valid(float target_c)
{
    if (!isfinite(target_c) || target_c < 0.0f || target_c > 80.0f) {
        return false;
    }
    const float doubled = target_c * 2.0f;
    return fabsf(doubled - roundf(doubled)) <= 0.001f;
}

bool heat_hysteresis_is_valid(float hysteresis_c)
{
    return hysteresis_c >= HEAT_MIN_HYSTERESIS_C && hysteresis_c <= HEAT_MAX_HYSTERESIS_C &&
           hysteresis_c * 2.0f == roundf(hysteresis_c * 2.0f);
}

bool heat_restart_is_valid(float target_c, float restart_c)
{
    return target_c > 0.0f && heat_hysteresis_is_valid(target_c - restart_c);
}

void heat_controller_init(heat_controller_t *controller,
                          const heat_controller_config_t *config)
{
    if (controller == NULL || config == NULL) {
        return;
    }
    memset(controller, 0, sizeof(*controller));
    controller->config = *config;
    if (!heat_hysteresis_is_valid(controller->config.hysteresis_c)) {
        controller->config.hysteresis_c = HEAT_DEFAULT_HYSTERESIS_C;
    }
    controller->snapshot = (heat_controller_snapshot_t) {
        .state = HEAT_STATE_BOOT,
        .fault = HEAT_FAULT_NONE,
        .enabled = false,
        .output_on = false,
        .target_c = heat_target_is_valid(config->target_c)
            ? config->target_c
            : 60.0f,
    };
}

void heat_controller_note_relay_off(heat_controller_t *controller, uint64_t now_ms)
{
    if (controller == NULL) {
        return;
    }
    controller->relay_off_at_ms = now_ms;
    controller->relay_off_recorded = true;
}

void heat_controller_step(heat_controller_t *controller,
                          const temperature_sample_t *sample,
                          uint64_t now_ms)
{
    if (controller == NULL || sample == NULL ||
        controller->snapshot.state == HEAT_STATE_FAULT) {
        return;
    }

    if (sample_has_overtemperature(controller, sample)) {
        latch_fault(controller, HEAT_FAULT_OVERTEMP);
        return;
    }
    if (!sample_is_healthy(sample)) {
        /* Only new acquisitions advance the retry count. Hold the existing
         * output during retries; stale-task and other faults remain immediate. */
        if (sample_is_retrying(sample)) {
            return;
        }
        latch_fault(controller, sample->fault);
        return;
    }
    if (!controller->snapshot.enabled) {
        return;
    }
    if (controller->snapshot.target_c <= 0.0f) {
        force_idle(controller);
        return;
    }
    if (controller->snapshot.inhibited ||
        sample->filtered_c >= controller->snapshot.target_c) {
        hold_output(controller);
        return;
    }

    if (!controller->snapshot.output_on &&
        sample->consecutive_valid >= 3U &&
        (controller->manual_start_pending ||
         sample->filtered_c <= controller->snapshot.target_c - controller->config.hysteresis_c) &&
        off_interval_elapsed(controller, now_ms)) {
        controller->snapshot.output_on = true;
        controller->manual_start_pending = false;
        controller->snapshot.state = HEAT_STATE_HEATING;
    }
}

static bool sample_is_cool(const temperature_sample_t *sample)
{
    return sample_is_healthy(sample) &&
           sample->probe_c < 80.0f && sample->raw_c < 80.0f && sample->filtered_c < 80.0f;
}

static bool fault_can_be_acknowledged(
    const heat_controller_t *controller,
    const temperature_sample_t *sample)
{
    if (controller->snapshot.restart_lockout &&
        (!sample_is_cool(sample) || sample->consecutive_valid < 3U)) {
        return false;
    }
    switch (controller->snapshot.fault) {
    case HEAT_FAULT_NVS:
    case HEAT_FAULT_RESTART_LOCKOUT:
        return true;
    case HEAT_FAULT_OUTPUT_INIT:
        return false;
    case HEAT_FAULT_OVERTEMP:
        return sample_is_cool(sample);
    case HEAT_FAULT_SENSOR_OPEN:
    case HEAT_FAULT_SENSOR_SHORT:
    case HEAT_FAULT_SENSOR_RANGE:
    case HEAT_FAULT_SENSOR_INVALID:
    case HEAT_FAULT_SENSOR_STALE:
        return sample_is_healthy(sample) &&
               sample->consecutive_valid >= 3U;
    case HEAT_FAULT_NONE:
    default:
        return false;
    }
}

bool heat_controller_command(heat_controller_t *controller,
                             const heat_controller_command_t *command,
                             const temperature_sample_t *sample,
                             uint64_t now_ms)
{
    if (controller == NULL || command == NULL) {
        return false;
    }

    switch (command->kind) {
    case HEAT_COMMAND_ENABLE:
        if (controller->snapshot.state == HEAT_STATE_FAULT ||
            controller->snapshot.target_c <= 0.0f ||
            !sample_is_healthy(sample) ||
            sample->consecutive_valid < 3U) {
            return false;
        }
        if (sample_has_overtemperature(controller, sample)) {
            latch_fault(controller, HEAT_FAULT_OVERTEMP);
            return false;
        }
        if (controller->snapshot.enabled) {
            return true;
        }
        controller->snapshot.enabled = true;
        controller->snapshot.state = HEAT_STATE_HOLDING;
        controller->manual_start_pending = true;
        heat_controller_step(controller, sample, now_ms);
        return true;

    case HEAT_COMMAND_DISABLE:
        force_idle(controller);
        return true;

    case HEAT_COMMAND_SET_TARGET:
        if (!heat_target_is_valid(command->target_c)) {
            return false;
        }
        controller->snapshot.target_c = command->target_c;
        controller->config.target_c = command->target_c;
        if (command->target_c == 0.0f) {
            force_idle(controller);
        } else if (controller->snapshot.enabled && sample != NULL) {
            heat_controller_step(controller, sample, now_ms);
        }
        return true;

    case HEAT_COMMAND_SET_RESTART:
        if (!heat_restart_is_valid(controller->snapshot.target_c, command->restart_c)) {
            return false;
        }
        controller->config.hysteresis_c = controller->snapshot.target_c - command->restart_c;
        return true;

    case HEAT_COMMAND_ACK_FAULT:
        if (controller->snapshot.state != HEAT_STATE_FAULT ||
            !fault_can_be_acknowledged(controller, sample)) {
            return false;
        }
        controller->snapshot.fault = HEAT_FAULT_NONE;
        controller->snapshot.restart_lockout = false;
        force_idle(controller);
        return true;

    case HEAT_COMMAND_LATCH_EXTERNAL_FAULT:
        if (command->fault != HEAT_FAULT_NVS &&
            command->fault != HEAT_FAULT_OUTPUT_INIT &&
            command->fault != HEAT_FAULT_RESTART_LOCKOUT) {
            return false;
        }
        latch_fault(controller, command->fault);
        return true;

    default:
        return false;
    }
}

void heat_controller_set_inhibit(heat_controller_t *controller, bool inhibited)
{
    if (controller == NULL) {
        return;
    }
    controller->snapshot.inhibited = inhibited;
    if (inhibited && controller->snapshot.enabled) {
        hold_output(controller);
    }
}

heat_controller_snapshot_t heat_controller_snapshot(
    const heat_controller_t *controller)
{
    if (controller == NULL) {
        return (heat_controller_snapshot_t) {
            .state = HEAT_STATE_FAULT,
            .fault = HEAT_FAULT_SENSOR_INVALID,
        };
    }
    heat_controller_snapshot_t snapshot = controller->snapshot;
    snapshot.restart_c = snapshot.target_c - controller->config.hysteresis_c;
    return snapshot;
}
