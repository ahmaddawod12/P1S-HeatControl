#include "printer_report.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void report_requires_complete_valid_json(void) {
  printer_report_delta_t d;
  const char *valid =
      "{\"print\":{\"command\":\"push_status\",\"gcode_state\":\"PAUSE\","
      "\"gcode_start_time\":\"123\",\"subtask_name\":\"part "
      "\\uD83D\\uDE80\",\"print_error\":0,\"hms\":[],\"layer_num\":0}}";
  assert(printer_report_parse(valid, strlen(valid), &d) == ESP_OK);
  assert(d.state == PRINTER_RUN_PAUSED && d.start_time == 123 &&
         !d.hms_blocking);
  assert(strcmp(d.label, "part \xF0\x9F\x9A\x80") == 0);
  const char *bad[] = {
      "{}",
      "{\"print\":{\"gcode_state\":\"PAUSE\",\"gcode_state\":\"RUNNING\"}}",
      "{\"print\":{\"print_error\":0,\"print_error\":1}}",
      "{\"print\":{\"gcode_state\":null}}",
      "{\"print\":{\"mc_percent\":NaN}}",
      "{\"print\":{\"ignored\":[tru]}}",
      "{\"print\":{\"subtask_name\":\"\\uD800\"}}",
      "{\"print\":{\"subtask_name\":\"\\u0000\"}}",
      "{\"print\":{\"gcode_state\":\"PAUSE\"}} garbage",
      "{\"print\":{},\"print\":{}}",
      "{\"print\":{\"subtask_name\":\"\xC0\x80\"}}"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
    assert(printer_report_parse(bad[i], strlen(bad[i]), &d) != ESP_OK);
  const char *unknown = "{\"print\":{\"gcode_state\":\"NEW_STATE\"}}";
  assert(printer_report_parse(unknown, strlen(unknown), &d) == ESP_OK &&
         d.state == PRINTER_RUN_UNKNOWN);
  const char *delta = "{\"print\":{\"mc_percent\":0}}";
  assert(printer_report_parse(delta, strlen(delta), &d) == ESP_OK);
  assert((d.present & PRINTER_FIELD_PROGRESS) &&
         !(d.present & PRINTER_FIELD_STATE));
  char deep[100];
  memset(deep, '[', 20);
  memset(deep + 20, ']', 20);
  deep[40] = 0;
  assert(printer_report_parse(deep, 40, &d) != ESP_OK);
  char *large = calloc(32770, 1);
  assert(large);
  memset(large, ' ', 32769);
  assert(printer_report_parse(large, 32769, &d) != ESP_OK);
  free(large);
}
static void fragment_offsets_are_checked(void) {
  printer_frame_t *f = calloc(1, sizeof(*f));
  assert(f);
  const char *out = NULL;
  size_t len = 0;
  printer_fragment_t p = {.topic = "device/ABC/report",
                          .topic_len = 17,
                          .data = "{",
                          .len = 1,
                          .total = 2,
                          .epoch = 7};
  assert(printer_frame_feed(f, &p, &out, &len) == PRINTER_FRAME_PENDING);
  p.topic = NULL;
  p.topic_len = 0;
  p.offset = 1;
  p.data = "}";
  assert(printer_frame_feed(f, &p, &out, &len) == PRINTER_FRAME_COMPLETE &&
         len == 2 && memcmp(out, "{}", 2) == 0);
  p.offset = 0;
  p.total = 32769;
  assert(printer_frame_feed(f, &p, &out, &len) == PRINTER_FRAME_REJECTED);
  p.total = 2;
  p.topic = "device/ABC/report";
  p.topic_len = 17;
  p.retained = true;
  assert(printer_frame_feed(f, &p, &out, &len) == PRINTER_FRAME_REJECTED);
  p.retained = false;
  assert(printer_frame_feed(f, &p, &out, &len) == PRINTER_FRAME_PENDING);
  p.offset = 1;
  p.epoch = 8;
  assert(printer_frame_feed(f, &p, &out, &len) == PRINTER_FRAME_REJECTED);
  free(f);
}
int main(void) {
  report_requires_complete_valid_json();
  fragment_offsets_are_checked();
  puts("printer reports passed");
}
