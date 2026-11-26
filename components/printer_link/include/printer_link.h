#pragma once
#include "esp_err.h"
#include "printer_types.h"
/* Local MQTT subscription to the P1S. Uses only gcode_state from reports. */
typedef struct {
  esp_err_t (*connect)(const printer_connection_config_t *, void *);
  esp_err_t (*subscribe)(const char *, void *);
  void (*abort)(void *);
  void *ctx;
} printer_link_ops_t;
esp_err_t printer_link_start(void);
esp_err_t printer_link_start_with_ops(const printer_link_ops_t *);
esp_err_t printer_link_configure(const printer_connection_config_t *);
void printer_link_tick(uint64_t now_ms);
/* Thread-safe snapshot including pending reports. Does not consume history. */
void printer_link_get_status(printer_link_status_t *out);
/* Immediate invalidation; resource teardown is acknowledged by quiescent after
 * tick. */
void printer_link_suspend(bool);
bool printer_link_quiescent(void);
