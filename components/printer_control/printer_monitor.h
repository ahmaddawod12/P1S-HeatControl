#pragma once
/* Monitor P1S: turns printer gcode_state into "heat allowed" and end decisions.
 * Pure logic; see docs/superpowers/specs/2026-11-07-monitor-p1s-design.md. */
#include "printer_types.h"

#define PRINTER_MONITOR_WAIT_MS 3600000ULL    /* no print started → end */
#define PRINTER_MONITOR_SILENCE_MS 300000ULL  /* active print, no report → lost */
#define PRINTER_MONITOR_PERMIT_MS 5000ULL     /* runtime heat permit length */

void printer_monitor_init(printer_monitor_status_t *m);
/* False when already on, disconnected or the printer state is unknown. */
bool printer_monitor_start(printer_monitor_status_t *m,
                           const printer_link_status_t *link, uint64_t now_ms);
void printer_monitor_step(printer_monitor_status_t *m,
                          const printer_link_status_t *link, uint64_t now_ms);
void printer_monitor_stop(printer_monitor_status_t *m,
                          printer_monitor_end_t reason);
bool printer_monitor_heat_allowed(const printer_monitor_status_t *m);
bool printer_monitor_end_disables_heating(printer_monitor_end_t reason);
uint64_t printer_monitor_wait_remaining(const printer_monitor_status_t *m,
                                        uint64_t now_ms);
