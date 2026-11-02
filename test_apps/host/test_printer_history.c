#include "printer_cycle.h"
#include "printer_report.h"
#include "printer_status.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char token[] = "11111111111111111111111111111111";
static void report(printer_status_t *s, const char *json, uint64_t now) {
  printer_report_delta_t delta;
  assert(printer_report_parse(json, strlen(json), &delta) == ESP_OK);
  assert(printer_status_apply(s, &delta, 7, now));
}
static void refresh(printer_status_t *s, printer_cycle_input_t *in,
                    uint64_t now) {
  in->now_ms = in->heat.temperature.timestamp_ms = now;
  in->automatic.expires_ms = now + 5000;
  printer_status_snapshot(s, now, &in->printer);
}
static void setup(printer_status_t *s, printer_cycle_input_t *in,
                  printer_cycle_t *cycle) {
  printer_status_init(s, 7);
  report(s,
         "{\"print\":{\"gcode_state\":\"PAUSE\",\"gcode_start_time\":100,"
         "\"gcode_file\":\"job.3mf\",\"layer_num\":0,\"print_error\":0,\"hms\":"
         "[]}}",
         1000);
  *in = (printer_cycle_input_t){.qualified = true};
  in->heat = app_status_initial();
  in->heat.target_c = 60;
  in->heat.temperature = (temperature_sample_t){
      .valid = true, .consecutive_valid = 3, .raw_c = 60, .filtered_c = 60};
  in->automatic.teardown_applied = true;
  refresh(s, in, 1000);
  printer_cycle_init(cycle);
  printer_cycle_offer_arm(cycle, in, token);
}
static void arm(printer_cycle_t *cycle, printer_cycle_input_t *in) {
  printer_arm_request_t request = {.initial_pause_confirmed = true};
  strcpy(request.arm_token, token);
  assert(printer_cycle_arm(cycle, &request, in, 7) == ESP_OK);
  in->heat.enabled = true;
  in->automatic.active = true;
  in->automatic.cycle_id = 7;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  printer_status_t status;
  printer_cycle_input_t in;
  printer_cycle_t cycle;
  printer_cycle_effects_t effects;
  setup(&status, &in, &cycle);
  if (!strcmp(argv[1], "benign")) {
    report(&status, "{\"print\":{\"print_type\":\"local\"}}", 1001);
    report(&status, "{\"print\":{\"print_type\":\"cloud\",\"mc_percent\":12}}",
           1002);
    refresh(&status, &in, 1003);
    printer_cycle_offer_arm(&cycle, &in, "22222222222222222222222222222222");
    assert(!strcmp(cycle.snapshot.arm_token, token));
    arm(&cycle, &in);
    return 0;
  }
  if (!strcmp(argv[1], "token")) {
    /* An error which clears before polling must still consume the old offer. */
    report(&status, "{\"print\":{\"print_error\":42}}", 1001);
    report(&status, "{\"print\":{\"print_error\":0}}", 1002);
    refresh(&status, &in, 1003);
    printer_arm_request_t request = {.initial_pause_confirmed = true};
    strcpy(request.arm_token, token);
    assert(printer_cycle_arm(&cycle, &request, &in, 7) == ESP_ERR_NOT_ALLOWED);
    const char next[] = "22222222222222222222222222222222";
    printer_cycle_offer_arm(&cycle, &in, next);
    strcpy(request.arm_token, next);
    assert(printer_cycle_arm(&cycle, &request, &in, 8) == ESP_OK);
    return 0;
  }
  arm(&cycle, &in);
  printer_cycle_step(&cycle, &in, &effects);
  bool manual = !strcmp(argv[1], "manual");
  if (!manual) {
    report(&status, "{\"print\":{\"gcode_state\":\"PAUSE\"}}", 31000);
    refresh(&status, &in, 31000);
    printer_cycle_step(&cycle, &in, &effects);
    assert(effects.send_resume);
    report(&status, "{\"print\":{\"gcode_state\":\"RUNNING\"}}", 32000);
    refresh(&status, &in, 32000);
    printer_cycle_step(&cycle, &in, &effects);
    assert(cycle.snapshot.state == PRINTER_CYCLE_PRINTING);
  }
  const uint64_t at = manual ? 2000 : 33000;
  printer_cycle_outcome_t expected;
  if (!strcmp(argv[1], "pause")) {
    report(&status, "{\"print\":{\"gcode_state\":\"PAUSE\"}}", at);
    report(&status, "{\"print\":{\"gcode_state\":\"RUNNING\"}}", at + 1);
    expected = PRINTER_OUTCOME_LATER_PAUSE;
  } else if (manual) {
    report(&status, "{\"print\":{\"gcode_state\":\"RUNNING\"}}", at);
    report(&status, "{\"print\":{\"gcode_state\":\"PAUSE\"}}", at + 1);
    expected = PRINTER_OUTCOME_CANCELLED;
  } else if (!strcmp(argv[1], "identity")) {
    report(&status, "{\"print\":{\"gcode_file\":\"other.3mf\"}}", at);
    report(&status, "{\"print\":{\"gcode_file\":\"job.3mf\"}}", at + 1);
    expected = PRINTER_OUTCOME_IDENTITY_CHANGED;
  } else {
    bool hms = !strcmp(argv[1], "hms");
    report(&status,
           hms ? "{\"print\":{\"hms\":[{\"attr\":1}]}}"
               : "{\"print\":{\"print_error\":42}}",
           at);
    report(&status,
           hms ? "{\"print\":{\"hms\":[]}}" : "{\"print\":{\"print_error\":0}}",
           at + 1);
    expected = PRINTER_OUTCOME_PRINTER_ERROR;
  }
  /* These are the same ordered deltas that a link queue drains before one poll.
   */
  refresh(&status, &in, at + 2);
  printer_cycle_step(&cycle, &in, &effects);
  assert(effects.revoke_heat && !effects.renew_heat && !effects.send_resume);
  assert(cycle.snapshot.outcome == expected);
  puts("adverse report history: ok");
  return 0;
}
