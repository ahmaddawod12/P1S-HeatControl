#include "printer_status.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void apply(printer_status_t *s, const char *json, uint64_t now) {
  printer_report_delta_t d;
  assert(printer_report_parse(json, strlen(json), &d) == ESP_OK);
  assert(printer_status_apply(s, &d, 7, now));
}
int main(void) {
  printer_status_t s;
  printer_observation_t o;
  printer_status_init(&s, 7);
  const char *pause = "{\"print\":{\"gcode_state\":\"PAUSE\",\"gcode_start_"
                      "time\":\"0\",\"task_id\":\"0\",\"subtask_name\":"
                      "\"same\",\"layer_num\":0,\"print_error\":0,\"hms\":[]}}";
  apply(&s, pause, 1000);
  printer_status_snapshot(&s, 1000, &o);
  assert(!o.startup_eligible);
  apply(&s, "{\"print\":{\"gcode_state\":\"IDLE\"}}", 1100);
  apply(&s, pause, 1200);
  printer_status_snapshot(&s, 1200, &o);
  assert(o.startup_eligible);
  printer_job_key_t first = o.job;
  apply(&s, "{\"print\":{\"mc_percent\":0}}", 1300);
  printer_status_snapshot(&s, 1300, &o);
  assert(o.state == PRINTER_RUN_PAUSED && o.startup_eligible &&
         printer_job_same(&first, &o.job));
  printer_status_snapshot(&s, 31299, &o);
  assert(o.fresh);
  printer_status_snapshot(&s, 31300, &o);
  assert(!o.fresh && !o.startup_eligible);
  apply(&s, "{\"print\":{\"gcode_state\":\"FINISH\"}}", 32000);
  apply(&s, pause, 32100);
  printer_status_snapshot(&s, 32100, &o);
  assert(o.startup_eligible && !printer_job_same(&first, &o.job));
  printer_status_init(&s, 8);
  printer_status_snapshot(&s, 32200, &o);
  assert(!o.baseline_valid && !o.startup_eligible);
  printer_report_delta_t d = {.present = PRINTER_FIELD_STATE,
                              .state = PRINTER_RUN_RUNNING};
  assert(!printer_status_apply(&s, &d, 7, 32200));
  puts("printer status passed");
}
