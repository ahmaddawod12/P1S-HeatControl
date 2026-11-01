#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_types.h"
#include "diagnostics.h"
#include "esp_err.h"

#define API_MAX_BODY_BYTES 256U
#define API_STATUS_JSON_BYTES 512U
#define API_SSE_FRAME_BYTES (API_STATUS_JSON_BYTES + 32U)
// Room for every ring entry, including long timestamps and fault names.
#define API_DIAGNOSTICS_JSON_BYTES (DIAGNOSTICS_CAPACITY * 128U + 16U)

/* Stable wire names; the browser and any script depend on these. */
const char *api_state_name(heat_state_t state);
const char *api_fault_name(heat_fault_t fault);

/* Requires a JSON object carrying exactly a numeric `target_c` that passes
 * heat_target_is_valid(). Returns ESP_ERR_INVALID_SIZE for bodies past
 * API_MAX_BODY_BYTES and leaves *out_target untouched on any failure. */
esp_err_t api_parse_target(const char *body, float *out_target);

esp_err_t api_status_json(const app_status_t *status,
                          char *out_json,
                          size_t capacity);

/* Writes `event: status\ndata: {...}\n\n`. */
esp_err_t api_status_event(const app_status_t *status,
                           char *out_frame,
                           size_t capacity,
                           size_t *out_length);

esp_err_t api_diagnostics_json(const diagnostic_record_t *records,
                               size_t count,
                               char *out_json,
                               size_t capacity);

esp_err_t api_error_json(const char *code,
                         const char *message,
                         char *out_json,
                         size_t capacity);
