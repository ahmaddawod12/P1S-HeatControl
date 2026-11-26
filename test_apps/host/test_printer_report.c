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
  printer_frame_clear(&frame);
  assert(frame.bytes == NULL);
  assert(printer_frame_feed(&frame, &b, &out, &len) == PRINTER_FRAME_REJECTED);
}

static void complete_reports_borrow_input(void) {
  printer_frame_t frame = {0};
  const char json[] = "{\"print\":{\"gcode_state\":\"IDLE\"}}";
  const char topic[] = "device/X/report";
  printer_fragment_t part = {
      .topic = topic, .topic_len = sizeof(topic) - 1U, .data = json,
      .total = sizeof(json) - 1U, .len = 5U, .epoch = 1U,
  };
  const char *report = NULL;
  size_t report_len = 0;
  assert(printer_frame_feed(&frame, &part, &report, &report_len) == PRINTER_FRAME_PENDING);
  assert(frame.bytes != NULL);

  /* A fresh report discards a partial frame from the old connection. */
  part.epoch = 2U;
  part.len = part.total;
  assert(printer_frame_feed(&frame, &part, &report, &report_len) == PRINTER_FRAME_COMPLETE);
  assert(report == json && report_len == sizeof(json) - 1U && frame.bytes == NULL);
  printer_frame_clear(&frame);
}

static void maximum_size_frames_reassemble_repeatedly(void) {
  static printer_frame_t frame;
  static char payload[PRINTER_REPORT_MAX];
  const char prefix[] = "{\"print\":{\"command\":\"push_status\",\"description\":\"";
  const char suffix[] = "\",\"gcode_state\":\"RUNNING\"}}";
  const size_t start = sizeof(prefix) - 1U;
  const size_t end = sizeof(payload) - (sizeof(suffix) - 1U);
  memcpy(payload, prefix, start);
  memset(payload + start, 'x', end - start);
  memcpy(payload + end, suffix, sizeof(suffix) - 1U);

  const char *report = NULL;
  size_t report_len = 0;
  const char topic[] = "device/X/report";
  const printer_fragment_t oversized = {
      .topic = topic, .topic_len = sizeof(topic) - 1U,
      .data = payload, .len = 1, .total = sizeof(payload) + 1U,
  };
  assert(printer_frame_feed(&frame, &oversized, &report, &report_len) ==
         PRINTER_FRAME_REJECTED);

  for (uint32_t epoch = 1; epoch <= 128U; ++epoch) {
    for (size_t offset = 0; offset < sizeof(payload);) {
      const size_t remaining = sizeof(payload) - offset;
      const size_t count = remaining > 1000U ? 1000U : remaining;
      const printer_fragment_t part = {
          .topic = offset == 0 ? topic : NULL,
          .topic_len = offset == 0 ? sizeof(topic) - 1U : 0U,
          .data = payload + offset, .len = count, .offset = offset,
          .total = sizeof(payload), .epoch = epoch,
      };
      const printer_frame_result_t expected =
          count == remaining ? PRINTER_FRAME_COMPLETE : PRINTER_FRAME_PENDING;
      assert(printer_frame_feed(&frame, &part, &report, &report_len) == expected);
      offset += count;
    }
    printer_report_t parsed;
    assert(report_len == sizeof(payload));
    assert(printer_report_parse(report, report_len, &parsed) == ESP_OK);
    assert(parsed.has_state && parsed.state == PRINTER_RUN_RUNNING);
    printer_frame_clear(&frame);
    assert(frame.bytes == NULL);
  }
}

int main(void) {
  each_gcode_state();
  full_report_yields_only_state();
  report_without_state();
  other_messages_not_found();
  malformed_reports_rejected();
  frames_reassemble();
  complete_reports_borrow_input();
  maximum_size_frames_reassemble_repeatedly();
  puts("printer_report: PASS");
  return 0;
}
