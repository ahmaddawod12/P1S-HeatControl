#include "heat_controller.h"

#include <math.h>
#include <string.h>

static bool sample_has_overtemperature(const heat_controller_t *controller,
                                       const temperature_sample_t *sample)
{
    return sample != NULL &&
           ((isfinite(sample->raw_c) &&
             sample->raw_c >= controller->config.overtemp_c) ||
            (isfinite(sample->filtered_c) &&
             sample->filtered_c >= controller->config.overtemp_c));
}

static bool sample_is_healthy(const temperature_sample_t *sample)
{
    return sample != NULL && sample->valid &&
           sample->fault == HEAT_FAULT_NONE &&
           isfinite(sample->raw_c) && isfinite(sample->filtered_c);
}

static void record_output(heat_controller_t *controller,
                          bool output_on,
                          uint64_t now_ms)
{
    if (controller->snapshot.output_on == output_on) {
        return;
    }
    controller->snapshot.output_on = output_on;
    controller->last_transition_ms = now_ms;
    controller->has_transition = true;
}

static void force_idle(heat_controller_t *controller, uint64_t now_ms)
{
    record_output(controller, false, now_ms);
    controller->snapshot.enabled = false;
    controller->snapshot.state = controller->snapshot.fault == HEAT_FAULT_NONE
        ? HEAT_STATE_IDLE : HEAT_STATE_FAULT;
}

static void latch_fault(heat_controller_t *controller,
                        heat_fault_t fault,
                        uint64_t now_ms)
{
    record_output(controller, false, now_ms);
    controller->snapshot.enabled = false;
    controller->snapshot.state = HEAT_STATE_FAULT;
    controller->snapshot.fault = fault == HEAT_FAULT_NONE
        ? HEAT_FAULT_SENSOR_INVALID
        : fault;
}

static bool transition_allowed(const heat_controller_t *controller,
                               uint64_t now_ms)
{
    return !controller->has_transition ||
           (now_ms - controller->last_transition_ms) >=
               controller->config.min_transition_ms;
}

bool heat_target_is_valid(float target_c)
{
    if (!isfinite(target_c) || target_c < 0.0f || target_c > 80.0f) {
        return false;
    }
    const float doubled = target_c * 2.0f;
    return fabsf(doubled - roundf(doubled)) <= 0.001f;
}

void heat_controller_init(heat_controller_t *controller,
                          const heat_controller_config_t *config)
{
    if (controller == NULL || config == NULL) {
        return;
    }
    memset(controller, 0, sizeof(*controller));
    controller->config = *config;
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

void heat_controller_step(heat_controller_t *controller,
                          const temperature_sample_t *sample,
                          uint64_t now_ms)
{
    if (controller == NULL || sample == NULL ||
        controller->snapshot.state == HEAT_STATE_FAULT) {
        return;
    }

    if (sample_has_overtemperature(controller, sample)) {
        latch_fault(controller, HEAT_FAULT_OVERTEMP, now_ms);
        return;
    }
    if (!sample_is_healthy(sample)) {
        latch_fault(controller, sample->fault, now_ms);
        return;
    }
    if (!controller->snapshot.enabled) {
        return;
    }
    if (controller->snapshot.target_c <= 0.0f) {
        force_idle(controller, now_ms);
        return;
    }

    if (sample->filtered_c >= controller->snapshot.target_c) {
        record_output(controller, false, now_ms);
        controller->snapshot.state = HEAT_STATE_HOLDING;
        return;
    }

    if (!controller->snapshot.output_on &&
        sample->filtered_c <=
            (controller->snapshot.target_c -
             controller->config.hysteresis_c)) {
        if (transition_allowed(controller, now_ms)) {
            record_output(controller, true, now_ms);
            controller->snapshot.state = HEAT_STATE_HEATING;
        }
    }
}

static bool fault_can_be_acknowledged(
    const heat_controller_t *controller,
    const temperature_sample_t *sample)
{
    switch (controller->snapshot.fault) {
    case HEAT_FAULT_NVS:
        return true;
    case HEAT_FAULT_OUTPUT_INIT:
        return false;
    case HEAT_FAULT_OVERTEMP:
        return sample_is_healthy(sample) &&
               sample->raw_c < 80.0f && sample->filtered_c < 80.0f;
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
            latch_fault(controller, HEAT_FAULT_OVERTEMP, now_ms);
            return false;
        }
        if (controller->snapshot.enabled) {
            return true;
        }
        controller->snapshot.enabled = true;
        if (sample->filtered_c >= controller->snapshot.target_c) {
            record_output(controller, false, now_ms);
            controller->snapshot.state = HEAT_STATE_HOLDING;
        } else {
            record_output(controller, true, now_ms);
            controller->snapshot.state = HEAT_STATE_HEATING;
        }
        return true;

    case HEAT_COMMAND_DISABLE:
        force_idle(controller, now_ms);
        return true;

    case HEAT_COMMAND_SET_TARGET:
        if (!heat_target_is_valid(command->target_c)) {
            return false;
        }
        controller->snapshot.target_c = command->target_c;
        controller->config.target_c = command->target_c;
        if (command->target_c == 0.0f) {
            force_idle(controller, now_ms);
        } else if (controller->snapshot.enabled && sample != NULL) {
            heat_controller_step(controller, sample, now_ms);
        }
        return true;

    case HEAT_COMMAND_ACK_FAULT:
        if (controller->snapshot.state != HEAT_STATE_FAULT ||
            !fault_can_be_acknowledged(controller, sample)) {
            return false;
        }
        controller->snapshot.fault = HEAT_FAULT_NONE;
        force_idle(controller, now_ms);
        return true;

    case HEAT_COMMAND_LATCH_EXTERNAL_FAULT:
        if (command->fault != HEAT_FAULT_NVS &&
            command->fault != HEAT_FAULT_OUTPUT_INIT) {
            return false;
        }
        latch_fault(controller, command->fault, now_ms);
        return true;

    default:
        return false;
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
    return controller->snapshot;
}
