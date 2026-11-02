#pragma once
#include "esp_err.h"
#include "printer_types.h"
#define PRINTER_API_MAX_BODY 1024U
#define PRINTER_API_MAX_RESPONSE 2048U
bool printer_api_parse_config(const char *, size_t, printer_config_request_t *);
bool printer_api_parse_arm(const char *, size_t, printer_arm_request_t *);
bool printer_api_empty_body(const char *, size_t);
esp_err_t printer_api_status_json(const printer_control_status_t *,
                                  const char *, char *, size_t);
