#include "printer_cycle.h"
#include "printer_status.h"
#include <math.h>
#include <string.h>
static bool active(const printer_cycle_t *c) {
  return c->snapshot.state == PRINTER_CYCLE_PREHEATING ||
         c->snapshot.state == PRINTER_CYCLE_RESUME_PENDING ||
         c->snapshot.state == PRINTER_CYCLE_PRINTING;
}
static bool heat_ok(const app_status_t *heat, uint64_t now) {
  const temperature_sample_t *t = &heat->temperature;
  return heat->fault == HEAT_FAULT_NONE && t->fault == HEAT_FAULT_NONE &&
         t->valid && t->consecutive_valid >= 3 && now >= t->timestamp_ms &&
         now - t->timestamp_ms <= TEMPERATURE_SAMPLE_MAX_AGE_MS &&
         isfinite(t->raw_c) && isfinite(t->filtered_c) && t->raw_c < 82 &&
         t->filtered_c < 82;
}
static bool thermal_ok(const printer_cycle_input_t *i) {
  return !i->maintenance && heat_ok(&i->heat, i->now_ms);
}
bool printer_cycle_dispatch_ready(const app_status_t *heat, float target,
                                  uint64_t now) {
  return heat && heat_ok(heat, now) && heat->enabled && isfinite(target) &&
         target > 0 && target <= 80 && heat->target_c == target &&
         fabsf(heat->temperature.filtered_c - target) <= 1;
}
static bool telemetry_ok(const printer_cycle_input_t *i) {
  return i->printer.connected && i->printer.fresh &&
         i->printer.baseline_valid && i->now_ms >= i->printer.received_ms &&
         i->now_ms - i->printer.received_ms < PRINTER_TELEMETRY_MS;
}
static bool same_history(const printer_safety_history_t *a,
                         const printer_safety_history_t *b) {
  return a->pause == b->pause && a->error == b->error &&
         a->identity == b->identity;
}
static bool eligible(const printer_cycle_t *c, const printer_cycle_input_t *i) {
  return !active(c) && i->qualified && thermal_ok(i) && telemetry_ok(i) &&
         i->printer.startup_eligible && !i->printer.blocking_error &&
         i->printer.state == PRINTER_RUN_PAUSED &&
         i->printer.job.identity_valid && !i->heat.enabled &&
         !i->automatic.active && i->automatic.teardown_applied &&
         isfinite(i->heat.target_c) && i->heat.target_c > 0 &&
         i->heat.target_c <= 80 &&
         !(c->snapshot.resume_attempted &&
           (printer_job_same(&c->snapshot.job, &i->printer.job) ||
            (c->snapshot.job.connection_epoch !=
                 i->printer.job.connection_epoch &&
             c->snapshot.job.start_time &&
             c->snapshot.job.start_time == i->printer.job.start_time)));
}
void printer_cycle_init(printer_cycle_t *c) { memset(c, 0, sizeof(*c)); }
void printer_cycle_offer_arm(printer_cycle_t *c, const printer_cycle_input_t *i,
                             const char token[33]) {
  if (!eligible(c, i) || UINT64_MAX - i->now_ms < 60000) {
    c->snapshot.arm_token[0] = 0;
    c->snapshot.token_expires_ms = 0;
    return;
  }
  if (c->snapshot.arm_token[0] && i->now_ms < c->snapshot.token_expires_ms &&
      c->offered_target == i->heat.target_c &&
      same_history(&c->offered_history, &i->printer.history) &&
      printer_job_same(&c->offered_job, &i->printer.job))
    return;
  if (!token || strlen(token) != 32) {
    c->snapshot.arm_token[0] = 0;
    return;
  }
  memcpy(c->snapshot.arm_token, token, 33);
  c->snapshot.token_expires_ms = i->now_ms + 60000;
  c->offered_job = i->printer.job;
  c->offered_history = i->printer.history;
  c->offered_target = i->heat.target_c;
}
esp_err_t printer_cycle_arm(printer_cycle_t *c, const printer_arm_request_t *r,
                            const printer_cycle_input_t *i, uint64_t id) {
  if (!r || !id || !r->initial_pause_confirmed ||
      !memchr(r->arm_token, 0, sizeof(r->arm_token)))
    return ESP_ERR_INVALID_ARG;
  if (!eligible(c, i) || !c->snapshot.arm_token[0] ||
      strcmp(r->arm_token, c->snapshot.arm_token) ||
      i->now_ms >= c->snapshot.token_expires_ms ||
      i->heat.target_c != c->offered_target ||
      !same_history(&c->offered_history, &i->printer.history) ||
      !printer_job_same(&c->offered_job, &i->printer.job) ||
      id <= c->snapshot.cycle_id || UINT64_MAX - i->now_ms < 2700000)
    return ESP_ERR_NOT_ALLOWED;
  c->snapshot = (printer_cycle_snapshot_t){.cycle_id = id,
                                           .state = PRINTER_CYCLE_PREHEATING,
                                           .target_c = i->heat.target_c,
                                           .job = i->printer.job,
                                           .connection_epoch =
                                               i->printer.connection_epoch,
                                           .deadline_ms = i->now_ms + 2700000};
  c->reached_target = false;
  c->armed_history = i->printer.history;
  c->settling = false;
  return ESP_OK;
}
void printer_cycle_stop(printer_cycle_t *c, printer_cycle_outcome_t reason,
                        uint64_t now) {
  (void)now;
  c->snapshot.arm_token[0] = 0;
  c->snapshot.token_expires_ms = 0;
  c->snapshot.outcome = reason;
  c->snapshot.state = reason == PRINTER_OUTCOME_COMPLETED
                          ? PRINTER_CYCLE_COMPLETED
                      : reason == PRINTER_OUTCOME_OPERATOR_STOP ||
                              reason == PRINTER_OUTCOME_LATER_PAUSE ||
                              reason == PRINTER_OUTCOME_CANCELLED ||
                              reason == PRINTER_OUTCOME_IDENTITY_CHANGED
                          ? PRINTER_CYCLE_STOPPED
                          : PRINTER_CYCLE_ERROR;
}
void printer_cycle_note_dispatch(printer_cycle_t *c,
                                 printer_dispatch_result_t result,
                                 uint64_t now) {
  if (c->snapshot.state != PRINTER_CYCLE_RESUME_PENDING)
    return;
  if (result != PRINTER_DISPATCH_SENT)
    printer_cycle_stop(c,
                       result == PRINTER_DISPATCH_REJECTED
                           ? PRINTER_OUTCOME_RESUME_REJECTED
                           : PRINTER_OUTCOME_RESUME_UNCERTAIN,
                       now);
}
void printer_cycle_step(printer_cycle_t *c, const printer_cycle_input_t *i,
                        printer_cycle_effects_t *e) {
  *e = (printer_cycle_effects_t){0};
  if (!active(c)) {
    e->revoke_heat = c->snapshot.cycle_id != 0;
    return;
  }
  printer_cycle_outcome_t fail = PRINTER_OUTCOME_NONE;
  if (!i->printer.connected)
    fail = PRINTER_OUTCOME_LINK_LOST;
  else if (!telemetry_ok(i))
    fail = PRINTER_OUTCOME_TELEMETRY_STALE;
  else if (!printer_job_same(&c->snapshot.job, &i->printer.job))
    fail = PRINTER_OUTCOME_IDENTITY_CHANGED;
  else if (i->printer.history.identity != c->armed_history.identity)
    fail = PRINTER_OUTCOME_IDENTITY_CHANGED;
  else if (!thermal_ok(i) || i->heat.target_c != c->snapshot.target_c)
    fail = PRINTER_OUTCOME_HEAT_FAULT;
  else if (i->printer.blocking_error || !i->qualified ||
           i->printer.history.error != c->armed_history.error)
    fail = PRINTER_OUTCOME_PRINTER_ERROR;
  else if (c->snapshot.state != PRINTER_CYCLE_PREHEATING &&
           i->printer.history.pause != c->armed_history.pause)
    fail = PRINTER_OUTCOME_LATER_PAUSE;
  else if (!i->automatic.active || i->automatic.expires_ms <= i->now_ms ||
           i->automatic.cycle_id != c->snapshot.cycle_id)
    fail = PRINTER_OUTCOME_LEASE_EXPIRED;
  if (fail != PRINTER_OUTCOME_NONE) {
    printer_cycle_stop(c, fail, i->now_ms);
    e->revoke_heat = true;
    return;
  }
  if (i->printer.state == PRINTER_RUN_FINISHED) {
    printer_cycle_stop(c, PRINTER_OUTCOME_COMPLETED, i->now_ms);
    e->revoke_heat = true;
    return;
  }
  if (i->printer.state == PRINTER_RUN_FAILED ||
      i->printer.state == PRINTER_RUN_IDLE ||
      i->printer.state == PRINTER_RUN_UNKNOWN) {
    printer_cycle_stop(c,
                       i->printer.state == PRINTER_RUN_FAILED
                           ? PRINTER_OUTCOME_PRINTER_ERROR
                           : PRINTER_OUTCOME_CANCELLED,
                       i->now_ms);
    e->revoke_heat = true;
    return;
  }
  if (c->snapshot.state == PRINTER_CYCLE_PREHEATING) {
    if (i->now_ms >= c->snapshot.deadline_ms) {
      printer_cycle_stop(c, PRINTER_OUTCOME_PREHEAT_TIMEOUT, i->now_ms);
      e->revoke_heat = true;
      return;
    }
    if (i->printer.state != PRINTER_RUN_PAUSED ||
        !i->printer.startup_eligible) {
      printer_cycle_stop(c, PRINTER_OUTCOME_CANCELLED, i->now_ms);
      e->revoke_heat = true;
      return;
    }
    float t = i->heat.temperature.filtered_c;
    if (t >= c->snapshot.target_c)
      c->reached_target = true;
    if (c->reached_target && fabsf(t - c->snapshot.target_c) <= 1) {
      if (!c->settling) {
        c->settling = true;
        c->settle_since = i->now_ms;
      }
    } else
      c->settling = false;
    if (c->settling && i->now_ms - c->settle_since >= 30000) {
      c->snapshot.resume_attempted = true;
      c->snapshot.state = PRINTER_CYCLE_RESUME_PENDING;
      c->snapshot.deadline_ms = i->now_ms + 30000;
      e->send_resume = true;
    }
  } else if (c->snapshot.state == PRINTER_CYCLE_RESUME_PENDING) {
    if (i->printer.state == PRINTER_RUN_RUNNING ||
        i->printer.state == PRINTER_RUN_PREPARING)
      c->snapshot.state = PRINTER_CYCLE_PRINTING;
    else if (i->now_ms >= c->snapshot.deadline_ms) {
      printer_cycle_stop(c, PRINTER_OUTCOME_RESUME_UNCERTAIN, i->now_ms);
      e->revoke_heat = true;
      return;
    }
  } else if (i->printer.state == PRINTER_RUN_PAUSED) {
    printer_cycle_stop(c, PRINTER_OUTCOME_LATER_PAUSE, i->now_ms);
    e->revoke_heat = true;
    return;
  }
  e->renew_heat = true;
}
void printer_cycle_snapshot(const printer_cycle_t *c,
                            printer_cycle_snapshot_t *out) {
  *out = c->snapshot;
}
