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
  char *bytes;
  size_t used, total;
  uint32_t epoch;
} printer_frame_t;
/* ESP_OK: a print report whose command is push_status (or absent);
 * has_state tells whether gcode_state was present. ESP_ERR_NOT_FOUND: valid
 * JSON without such a report. Other errors: malformed. */
esp_err_t printer_report_parse(const char *json, size_t len,
                               printer_report_t *out);
/* A complete frame borrows the incoming bytes or the assembly buffer until
 * the caller finishes reading it. Clear the frame after use or on disconnect. */
printer_frame_result_t printer_frame_feed(printer_frame_t *frame,
                                          const printer_fragment_t *part,
                                          const char **json, size_t *len);
void printer_frame_clear(printer_frame_t *frame);
