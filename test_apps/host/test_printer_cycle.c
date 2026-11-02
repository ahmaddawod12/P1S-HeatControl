#include "printer_cycle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char token1[] = "11111111111111111111111111111111";
static const char token2[] = "22222222222222222222222222222222";
static printer_cycle_input_t input(void) {
  printer_cycle_input_t i = {.now_ms = 1000, .qualified = true};
  i.heat = app_status_initial();
  i.heat.state = HEAT_STATE_IDLE;
  i.heat.target_c = 60;
  i.heat.temperature = (temperature_sample_t){.valid = true,
                                              .consecutive_valid = 3,
                                              .timestamp_ms = 1000,
                                              .raw_c = 25,
                                              .filtered_c = 25};
  i.automatic.teardown_applied = true;
  i.printer = (printer_observation_t){.connected = true,
                                      .fresh = true,
                                      .baseline_valid = true,
                                      .startup_eligible = true,
                                      .received_ms = 1000,
                                      .state = PRINTER_RUN_PAUSED,
                                      .connection_epoch = 1,
                                      .job = {.identity_valid = true,
                                              .connection_epoch = 1,
                                              .print_epoch = 1,
                                              .start_time = 123,
                                              .label = "same"}};
  return i;
}
static void time_at(printer_cycle_input_t *i, uint64_t now, float t) {
  i->now_ms = now;
  i->printer.received_ms = now;
  i->heat.temperature.timestamp_ms = now;
  i->heat.temperature.raw_c = t;
  i->heat.temperature.filtered_c = t;
  if(i->automatic.active)i->automatic.expires_ms=now+5000;
}
static void arm(printer_cycle_t *c, printer_cycle_input_t *i) {
  printer_cycle_init(c);
  printer_cycle_offer_arm(c, i, token1);
  printer_arm_request_t r = {.initial_pause_confirmed = true};
  strcpy(r.arm_token, token1);
  assert(printer_cycle_arm(c, &r, i, 7) == ESP_OK);
  assert(printer_cycle_arm(c, &r, i, 8) != ESP_OK);
  i->automatic.active = true;
  i->automatic.cycle_id = 7;
  i->automatic.expires_ms = i->now_ms+5000;
  i->heat.enabled = true;
}
static void readiness_is_continuous_and_once(void) {
  printer_cycle_t c;
  printer_cycle_input_t i = input();
  printer_cycle_effects_t e;
  printer_cycle_init(&c);
  printer_cycle_step(&c, &i, &e);
  assert(!e.send_resume && !e.renew_heat);
  arm(&c, &i);
  time_at(&i, 2000, 59.5f);
  printer_cycle_step(&c, &i, &e);
  time_at(&i, 40000, 59.5f);
  printer_cycle_step(&c, &i, &e);
  assert(!e.send_resume);
  time_at(&i, 41000, 60);
  printer_cycle_step(&c, &i, &e);
  assert(!e.send_resume);
  time_at(&i, 70999, 60);
  printer_cycle_step(&c, &i, &e);
  assert(!e.send_resume);
  time_at(&i, 71000, 60);
  printer_cycle_step(&c, &i, &e);
  assert(e.send_resume);
  printer_cycle_snapshot_t s;
  printer_cycle_snapshot(&c, &s);
  assert(s.resume_attempted);
  printer_cycle_step(&c, &i, &e);
  assert(!e.send_resume);
  printer_cycle_note_dispatch(&c, PRINTER_DISPATCH_SENT, i.now_ms);
  i.printer.state = PRINTER_RUN_RUNNING;
  i.printer.progress = 100;
  time_at(&i, 72000, 60);
  printer_cycle_step(&c, &i, &e);
  assert(!e.send_resume && !e.revoke_heat);
  i.printer.state = PRINTER_RUN_PAUSED;
  printer_cycle_step(&c, &i, &e);
  assert(e.revoke_heat && !e.send_resume);
  i.printer.state = PRINTER_RUN_RUNNING;
  printer_cycle_step(&c, &i, &e);
  assert(!e.send_resume && !e.renew_heat);
}
static void tokens_and_settling_boundaries(void) {
  printer_cycle_t c;
  printer_cycle_input_t i = input();
  printer_cycle_snapshot_t s;
  printer_cycle_effects_t e;
  printer_cycle_init(&c);
  i.qualified = false;
  printer_cycle_offer_arm(&c, &i, token1);
  printer_cycle_snapshot(&c, &s);
  assert(!s.arm_token[0]);
  i.qualified = true;
  printer_cycle_offer_arm(&c, &i, token1);
  printer_cycle_offer_arm(&c, &i, token2);
  printer_cycle_snapshot(&c, &s);
  assert(!strcmp(s.arm_token, token1));
  time_at(&i, 61000, 25);
  printer_arm_request_t r = {.initial_pause_confirmed = true};
  strcpy(r.arm_token, token1);
  assert(printer_cycle_arm(&c, &r, &i, 7) != ESP_OK);
  i = input();
  arm(&c, &i);
  time_at(&i, 1000, 60);
  printer_cycle_step(&c, &i, &e);
  time_at(&i, 20000, 58.9f);
  printer_cycle_step(&c, &i, &e);
  time_at(&i, 21000, 59);
  printer_cycle_step(&c, &i, &e);
  time_at(&i, 50999, 59);
  printer_cycle_step(&c, &i, &e);
  assert(!e.send_resume);
  time_at(&i, 51000, 59);
  printer_cycle_step(&c, &i, &e);
  assert(e.send_resume);
  i = input();
  arm(&c, &i);
  time_at(&i, 2701000, 60);
  printer_cycle_step(&c, &i, &e);
  printer_cycle_snapshot(&c, &s);
  assert(e.revoke_heat && s.state == PRINTER_CYCLE_ERROR &&
         s.outcome == PRINTER_OUTCOME_PREHEAT_TIMEOUT);
}
static void failures_revoke_and_never_retry(void) {
  for (unsigned mode = 0; mode < 11; mode++) {
    printer_cycle_t c;
    printer_cycle_input_t i = input();
    printer_cycle_effects_t e;
    arm(&c, &i);
    switch (mode) {
    case 0:
      i.printer.connected = false;
      break;
    case 1:
      i.now_ms += 30000;
      i.heat.temperature.timestamp_ms = i.now_ms;
      break;
    case 2:
      i.printer.job.print_epoch++;
      break;
    case 3:
      i.heat.temperature.raw_c = 82;
      break;
    case 4:
      i.heat.temperature.valid = false;
      break;
    case 5:
      i.printer.blocking_error = true;
      break;
    case 6:
      i.maintenance = true;
      break;
    case 7:
      i.automatic.active = false;
      break;
    case 8:
      i.printer.state = PRINTER_RUN_RUNNING;
      break;
    case 9:
      i.heat.target_c = 61;
      break;
    case 10:
      i.automatic.expires_ms=i.now_ms;
      break;
    }
    printer_cycle_step(&c, &i, &e);
    assert(e.revoke_heat && !e.send_resume);
  }
  printer_cycle_t c;
  printer_cycle_input_t i = input();
  printer_cycle_effects_t e;
  arm(&c, &i);
  time_at(&i, 1000, 60);
  printer_cycle_step(&c, &i, &e);
  time_at(&i, 31000, 60);
  printer_cycle_step(&c, &i, &e);
  assert(e.send_resume);
  printer_cycle_note_dispatch(&c, PRINTER_DISPATCH_UNKNOWN, 31000);
  printer_cycle_step(&c, &i, &e);
  assert(e.revoke_heat && !e.send_resume);
  i.automatic.active = false;
  i.heat.enabled = false;
  printer_cycle_offer_arm(&c, &i, token2);
  printer_cycle_snapshot_t s;
  printer_cycle_snapshot(&c, &s);
  assert(!s.arm_token[0]);
  i.printer.connection_epoch++;
  i.printer.job.connection_epoch++;
  printer_cycle_offer_arm(&c,&i,token2);printer_cycle_snapshot(&c,&s);
  assert(!s.arm_token[0]); /* reconnect does not authorize a retry of the same run */
  i.printer.job.start_time++;
  printer_cycle_offer_arm(&c,&i,token2);printer_cycle_snapshot(&c,&s);
  assert(s.arm_token[0]);
  i = input();
  arm(&c, &i);
  time_at(&i, 1000, 60);
  printer_cycle_step(&c, &i, &e);
  time_at(&i, 31000, 60);
  printer_cycle_step(&c, &i, &e);
  i.printer.state = PRINTER_RUN_FINISHED;
  printer_cycle_step(&c, &i, &e);
  printer_cycle_snapshot(&c, &s);
  assert(s.state == PRINTER_CYCLE_COMPLETED && e.revoke_heat);
  i = input();
  arm(&c, &i);
  time_at(&i, 1000, 60);
  printer_cycle_step(&c, &i, &e);
  time_at(&i, 31000, 60);
  printer_cycle_step(&c, &i, &e);
  time_at(&i, 61000, 60);
  printer_cycle_step(&c, &i, &e);
  printer_cycle_snapshot(&c, &s);
  assert(s.outcome == PRINTER_OUTCOME_RESUME_UNCERTAIN && e.revoke_heat);
}
int main(void) {
  readiness_is_continuous_and_once();
  tokens_and_settling_boundaries();
  failures_revoke_and_never_retry();
  puts("printer cycle passed");
}
