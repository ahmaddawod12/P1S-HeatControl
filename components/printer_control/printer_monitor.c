#include "printer_monitor.h"
#include <string.h>

static bool active(printer_run_state_t s) {
  return s == PRINTER_RUN_PREPARING || s == PRINTER_RUN_RUNNING ||
         s == PRINTER_RUN_PAUSED;
}
static bool known(const printer_link_status_t *link) {
  return link->state_known && link->state != PRINTER_RUN_UNKNOWN;
}
void printer_monitor_init(printer_monitor_status_t *m) {
  memset(m, 0, sizeof(*m));
}
bool printer_monitor_start(printer_monitor_status_t *m,
                           const printer_link_status_t *link, uint64_t now_ms) {
  if (!m || !link || m->phase != PRINTER_MONITOR_OFF || !link->connected ||
      !known(link))
    return false;
  /* A FINISH/FAILED/IDLE showing now is left over from the previous print. */
  m->phase = active(link->state) ? PRINTER_MONITOR_WATCHING
                                 : PRINTER_MONITOR_WAITING;
  m->hold = link->state == PRINTER_RUN_PAUSED ? PRINTER_HOLD_PAUSED
                                              : PRINTER_HOLD_NONE;
  m->last_end = PRINTER_MONITOR_END_NONE;
  m->started_ms = now_ms;
  return true;
}
void printer_monitor_stop(printer_monitor_status_t *m,
                          printer_monitor_end_t reason) {
  m->phase = PRINTER_MONITOR_OFF;
  m->hold = PRINTER_HOLD_NONE;
  m->last_end = reason;
}
void printer_monitor_step(printer_monitor_status_t *m,
                          const printer_link_status_t *link, uint64_t now_ms) {
  if (!m || !link || m->phase == PRINTER_MONITOR_OFF)
    return;
  const printer_run_state_t s = link->state;
  if (!link->connected) {
    m->hold = PRINTER_HOLD_DISCONNECTED;
  } else if (!known(link)) {
    m->hold = PRINTER_HOLD_UNKNOWN;
  } else if (m->phase == PRINTER_MONITOR_WATCHING &&
             (s == PRINTER_RUN_PREPARING || s == PRINTER_RUN_RUNNING) &&
             (now_ms < link->received_ms ? 0 : now_ms - link->received_ms) >=
                 PRINTER_MONITOR_SILENCE_MS) {
    m->hold = PRINTER_HOLD_DISCONNECTED;
  } else if (active(s)) {
    m->phase = PRINTER_MONITOR_WATCHING;
    m->hold = s == PRINTER_RUN_PAUSED ? PRINTER_HOLD_PAUSED : PRINTER_HOLD_NONE;
  } else if (m->phase == PRINTER_MONITOR_WATCHING) {
    printer_monitor_stop(m, s == PRINTER_RUN_FINISHED ? PRINTER_MONITOR_END_FINISHED
                            : s == PRINTER_RUN_FAILED ? PRINTER_MONITOR_END_FAILED
                                                      : PRINTER_MONITOR_END_IDLE);
    return;
  } else {
    m->hold = PRINTER_HOLD_NONE;
  }
  if (m->phase == PRINTER_MONITOR_WAITING &&
      printer_monitor_wait_remaining(m, now_ms) == 0)
    printer_monitor_stop(m, PRINTER_MONITOR_END_NO_PRINT);
}
bool printer_monitor_heat_allowed(const printer_monitor_status_t *m) {
  return m && m->phase != PRINTER_MONITOR_OFF && m->hold == PRINTER_HOLD_NONE;
}
bool printer_monitor_end_disables_heating(printer_monitor_end_t reason) {
  return reason == PRINTER_MONITOR_END_FINISHED ||
         reason == PRINTER_MONITOR_END_FAILED ||
         reason == PRINTER_MONITOR_END_IDLE ||
         reason == PRINTER_MONITOR_END_NO_PRINT ||
         reason == PRINTER_MONITOR_END_SETTINGS;
}
uint64_t printer_monitor_wait_remaining(const printer_monitor_status_t *m,
                                        uint64_t now_ms) {
  if (!m || m->phase != PRINTER_MONITOR_WAITING)
    return 0;
  const uint64_t elapsed = now_ms > m->started_ms ? now_ms - m->started_ms : 0;
  return elapsed >= PRINTER_MONITOR_WAIT_MS ? 0 : PRINTER_MONITOR_WAIT_MS - elapsed;
}
