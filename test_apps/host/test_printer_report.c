/* Monitor P1S reads only print.gcode_state from push_status reports. */
#include "printer_report.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static esp_err_t parse(const char *json, printer_report_t *out) {
  return printer_report_parse(json, strlen(json), out);
}
static void each_gcode_state(void) {
  const struct {
    const char *name;
    printer_run_state_t state;
  } cases[] = {{"IDLE", PRINTER_RUN_IDLE},       {"PREPARE", PRINTER_RUN_PREPARING},
               {"SLICING", PRINTER_RUN_PREPARING}, {"RUNNING", PRINTER_RUN_RUNNING},
               {"PAUSE", PRINTER_RUN_PAUSED},      {"FINISH", PRINTER_RUN_FINISHED},
               {"FAILED", PRINTER_RUN_FAILED},     {"NEW_STATE", PRINTER_RUN_UNKNOWN}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    char json[128];
    snprintf(json, sizeof(json),
             "{\"print\":{\"command\":\"push_status\",\"gcode_state\":\"%s\"}}",
             cases[i].name);
    printer_report_t r = {0};
    assert(parse(json, &r) == ESP_OK);
    assert(r.has_state && r.state == cases[i].state);
  }
}
static void full_report_yields_only_state(void) {
  const char *pushall =
      "{\"print\":{\"command\":\"push_status\",\"msg\":0,\"sequence_id\":\"0\","
      "\"gcode_state\":\"RUNNING\",\"gcode_file\":\"job.3mf\","
      "\"subtask_name\":\"part \\uD83D\\uDE80\",\"task_id\":\"123\","
      "\"gcode_start_time\":\"1700000000\",\"mc_percent\":42,"
      "\"mc_remaining_time\":17,\"layer_num\":12,\"bed_temper\":60.5,"
      "\"nozzle_temper\":220,\"print_error\":0,\"hms\":[{\"attr\":1,\"code\":2}],"
      "\"ams\":{\"ams\":[{\"id\":\"0\",\"tray\":[{\"id\":\"0\",\"tray_type\":\"PLA\"}]}]},"
      "\"lights_report\":[{\"node\":\"chamber_light\",\"mode\":\"on\"}],"
      "\"wifi_signal\":\"-48dBm\",\"print_type\":\"local\"}}";
  printer_report_t r = {0};
  assert(parse(pushall, &r) == ESP_OK);
  assert(r.has_state && r.state == PRINTER_RUN_RUNNING);
}
static void report_without_state(void) {
  printer_report_t r = {.has_state = true, .state = PRINTER_RUN_PAUSED};
  assert(parse("{\"print\":{\"command\":\"push_status\",\"mc_percent\":43}}", &r) ==
         ESP_OK);
  assert(!r.has_state);
  assert(parse("{\"print\":{\"bed_temper\":61}}", &r) == ESP_OK && !r.has_state);
}
static void other_messages_not_found(void) {
  printer_report_t r;
  assert(parse("{\"print\":{\"command\":\"project_file\",\"gcode_state\":\"RUNNING\"}}",
               &r) == ESP_ERR_NOT_FOUND);
  assert(parse("{\"info\":{\"command\":\"get_version\"}}", &r) == ESP_ERR_NOT_FOUND);
  assert(parse("{}", &r) == ESP_ERR_NOT_FOUND);
}
static void malformed_reports_rejected(void) {
  const char *bad[] = {
      "{\"print\":{\"command\":\"push_status\",\"gcode_state\":\"RUN",
      "{\"print\":{\"gcode_state\":\"PAUSE\",\"gcode_state\":\"RUNNING\"}}",
      "{\"print\":{\"gcode_state\":null}}",
      "{\"print\":{\"gcode_state\":3}}",
      "{\"print\":{\"mc_percent\":NaN}}",
      "{\"print\":{\"ignored\":[tru]}}",
      "{\"print\":{\"subtask_name\":\"\\uD800\"}}",
      "{\"print\":{\"subtask_name\":\"\\u0000\"}}",
      "{\"print\":{\"gcode_state\":\"PAUSE\"}} garbage",
      "{\"print\":{},\"print\":{}}",
      "{\"print\":{\"subtask_name\":\"\xC0\x80\"}}",
      "[1]"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
    printer_report_t r;
    const esp_err_t e = parse(bad[i], &r);
    assert(e != ESP_OK && e != ESP_ERR_NOT_FOUND);
  }
}
static void frames_reassemble(void) {
  static printer_frame_t frame;
  const char *json = "{\"print\":{\"gcode_state\":\"FINISH\"}}";
  const size_t total = strlen(json);
  printer_fragment_t a = {.topic = "device/X/report", .topic_len = 15, .data = json,
                          .len = 10, .offset = 0, .total = total, .epoch = 1};
  printer_fragment_t b = {.data = json + 10, .len = total - 10, .offset = 10,
                          .total = total, .epoch = 1};
  const char *out = NULL;
  size_t len = 0;
  assert(printer_frame_feed(&frame, &a, &out, &len) == PRINTER_FRAME_PENDING);
  assert(printer_frame_feed(&frame, &b, &out, &len) == PRINTER_FRAME_COMPLETE);
  printer_report_t r;
  assert(len == total && printer_report_parse(out, len, &r) == ESP_OK);
  assert(r.state == PRINTER_RUN_FINISHED);
  assert(printer_frame_feed(&frame, &b, &out, &len) == PRINTER_FRAME_REJECTED);
}
int main(void) {
  each_gcode_state();
  full_report_yields_only_state();
  report_without_state();
  other_messages_not_found();
  malformed_reports_rejected();
  frames_reassemble();
  puts("printer_report: PASS");
  return 0;
}
