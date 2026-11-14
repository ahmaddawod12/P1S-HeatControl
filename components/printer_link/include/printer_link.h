#pragma once
#include "esp_err.h"
#include "printer_types.h"
/* Local MQTT link to the P1S. Reads only gcode_state; the only message it
 * sends is the pushall status request (at most once per 5 minutes). */
typedef struct {
  esp_err_t (*connect)(const printer_connection_config_t *, void *);
  esp_err_t (*subscribe)(const char *, void *);
  /* ESP_OK once written before deadline_ms; otherwise an error. */
  esp_err_t (*publish)(const char *topic, const char *json, size_t len,
                       uint64_t deadline_ms, void *);
  void (*abort)(void *);
  uint64_t (*now_ms)(void *);
  void *ctx;
} printer_link_ops_t;
#define PRINTER_PUSHALL_MIN_INTERVAL_MS 300000U
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
