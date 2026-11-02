#pragma once
#include "esp_err.h"
#include "printer_types.h"
typedef enum {
  PRINTER_FRAME_PENDING,
  PRINTER_FRAME_COMPLETE,
  PRINTER_FRAME_REJECTED
} printer_frame_result_t;
typedef struct {
  const char *topic;
  size_t topic_len;
  const char *data;
  size_t len, offset, total;
  uint32_t epoch;
  bool retained;
} printer_fragment_t;
typedef struct {
  char bytes[PRINTER_REPORT_MAX + 1];
  size_t used, total;
  uint32_t epoch;
  bool active;
} printer_frame_t;
esp_err_t printer_report_parse(const char *json, size_t len,
                               printer_report_delta_t *out);
printer_frame_result_t printer_frame_feed(printer_frame_t *frame,
                                          const printer_fragment_t *part,
                                          const char **json, size_t *len);
