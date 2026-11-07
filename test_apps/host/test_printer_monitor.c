/* Monitor P1S rules from docs/superpowers/specs/2026-11-07-monitor-p1s-design.md. */
#include "printer_monitor.h"
#include <assert.h>
#include <stdio.h>

static printer_link_status_t link(bool connected, bool known,
                                  printer_run_state_t state, uint64_t received) {
  return (printer_link_status_t){.connected = connected, .state_known = known,
                                 .state = state, .received_ms = received};
}
static printer_link_status_t at(printer_run_state_t state, uint64_t received) {
  return link(true, true, state, received);
}
static printer_monitor_status_t started(printer_run_state_t state, uint64_t now) {
  printer_monitor_status_t m;
  printer_monitor_init(&m);
  const printer_link_status_t l = at(state, now);
  assert(printer_monitor_start(&m, &l, now));
  return m;
}
static void step(printer_monitor_status_t *m, printer_run_state_t state, uint64_t now) {
  const printer_link_status_t l = at(state, now);
  printer_monitor_step(m, &l, now);
}

static void start_refused_when_disconnected_or_unknown(void) {
  printer_monitor_status_t m;
  printer_monitor_init(&m);
  printer_link_status_t l = link(false, true, PRINTER_RUN_RUNNING, 0);
  assert(!printer_monitor_start(&m, &l, 10));
  l = link(true, false, PRINTER_RUN_UNKNOWN, 0);
  assert(!printer_monitor_start(&m, &l, 10));
  l = link(true, true, PRINTER_RUN_UNKNOWN, 0);
  assert(!printer_monitor_start(&m, &l, 10));
  assert(m.phase == PRINTER_MONITOR_OFF);
}
static void start_refused_when_already_on(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_RUNNING, 0);
  const printer_link_status_t l = at(PRINTER_RUN_RUNNING, 5);
  assert(!printer_monitor_start(&m, &l, 5));
}
static void start_states(void) {
  const printer_run_state_t waiting[] = {PRINTER_RUN_FINISHED, PRINTER_RUN_FAILED,
                                         PRINTER_RUN_IDLE};
  for (unsigned i = 0; i < 3; ++i) {
    printer_monitor_status_t m = started(waiting[i], 100);
    assert(m.phase == PRINTER_MONITOR_WAITING && m.hold == PRINTER_HOLD_NONE);
    assert(m.last_end == PRINTER_MONITOR_END_NONE && m.started_ms == 100);
    assert(printer_monitor_heat_allowed(&m));
  }
  printer_monitor_status_t m = started(PRINTER_RUN_PREPARING, 0);
  assert(m.phase == PRINTER_MONITOR_WATCHING && m.hold == PRINTER_HOLD_NONE);
  m = started(PRINTER_RUN_RUNNING, 0);
  assert(m.phase == PRINTER_MONITOR_WATCHING && m.hold == PRINTER_HOLD_NONE);
  m = started(PRINTER_RUN_PAUSED, 0);
  assert(m.phase == PRINTER_MONITOR_WATCHING && m.hold == PRINTER_HOLD_PAUSED);
  assert(!printer_monitor_heat_allowed(&m));
}
static void leftover_finish_then_new_print(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_FINISHED, 0);
  step(&m, PRINTER_RUN_FINISHED, 1000);
  assert(m.phase == PRINTER_MONITOR_WAITING && printer_monitor_heat_allowed(&m));
  step(&m, PRINTER_RUN_PREPARING, 2000);
  assert(m.phase == PRINTER_MONITOR_WATCHING && printer_monitor_heat_allowed(&m));
  step(&m, PRINTER_RUN_RUNNING, 3000);
  assert(printer_monitor_heat_allowed(&m));
  step(&m, PRINTER_RUN_FINISHED, 4000);
  assert(m.phase == PRINTER_MONITOR_OFF && m.last_end == PRINTER_MONITOR_END_FINISHED);
  assert(!printer_monitor_heat_allowed(&m));
  assert(printer_monitor_end_disables_heating(m.last_end));
}
static void pause_holds_and_resumes(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_RUNNING, 0);
  step(&m, PRINTER_RUN_PAUSED, 1000);
  assert(m.phase == PRINTER_MONITOR_WATCHING && m.hold == PRINTER_HOLD_PAUSED);
  assert(!printer_monitor_heat_allowed(&m));
  step(&m, PRINTER_RUN_RUNNING, 2000);
  assert(m.hold == PRINTER_HOLD_NONE && printer_monitor_heat_allowed(&m));
}
static void failed_from_paused_and_prepare(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_RUNNING, 0);
  step(&m, PRINTER_RUN_PAUSED, 1000);
  step(&m, PRINTER_RUN_FAILED, 2000);
  assert(m.phase == PRINTER_MONITOR_OFF && m.last_end == PRINTER_MONITOR_END_FAILED);
  m = started(PRINTER_RUN_FINISHED, 0);
  step(&m, PRINTER_RUN_PREPARING, 1000);
  step(&m, PRINTER_RUN_FAILED, 2000);
  assert(m.phase == PRINTER_MONITOR_OFF && m.last_end == PRINTER_MONITOR_END_FAILED);
}
static void idle_after_active_ends(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_RUNNING, 0);
  step(&m, PRINTER_RUN_IDLE, 1000);
  assert(m.phase == PRINTER_MONITOR_OFF && m.last_end == PRINTER_MONITOR_END_IDLE);
  assert(printer_monitor_end_disables_heating(m.last_end));
}
static void wait_timeout(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_IDLE, 0);
  step(&m, PRINTER_RUN_IDLE, 3599999);
  assert(m.phase == PRINTER_MONITOR_WAITING);
  assert(printer_monitor_wait_remaining(&m, 3599999) == 1);
  step(&m, PRINTER_RUN_IDLE, 3600000);
  assert(m.phase == PRINTER_MONITOR_OFF && m.last_end == PRINTER_MONITOR_END_NO_PRINT);
  assert(printer_monitor_end_disables_heating(m.last_end));
  assert(printer_monitor_wait_remaining(&m, 3600000) == 0);

  m = started(PRINTER_RUN_IDLE, 0);
  const printer_link_status_t down = link(false, false, PRINTER_RUN_UNKNOWN, 0);
  printer_monitor_step(&m, &down, 3600000);
  assert(m.phase == PRINTER_MONITOR_OFF && m.last_end == PRINTER_MONITOR_END_NO_PRINT);

  m = started(PRINTER_RUN_RUNNING, 0);
  step(&m, PRINTER_RUN_RUNNING, 7200000);
  assert(m.phase == PRINTER_MONITOR_WATCHING);
  assert(printer_monitor_wait_remaining(&m, 7200000) == 0);
}
static void disconnect_holds_then_recovers(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_RUNNING, 0);
  const printer_link_status_t down = link(false, false, PRINTER_RUN_UNKNOWN, 0);
  printer_monitor_step(&m, &down, 1000);
  assert(m.phase == PRINTER_MONITOR_WATCHING && m.hold == PRINTER_HOLD_DISCONNECTED);
  assert(!printer_monitor_heat_allowed(&m));
  step(&m, PRINTER_RUN_RUNNING, 2000);
  assert(m.hold == PRINTER_HOLD_NONE && printer_monitor_heat_allowed(&m));

  m = started(PRINTER_RUN_FINISHED, 0);
  printer_monitor_step(&m, &down, 1000);
  assert(m.phase == PRINTER_MONITOR_WAITING && m.hold == PRINTER_HOLD_DISCONNECTED);
  step(&m, PRINTER_RUN_FINISHED, 2000);
  assert(m.phase == PRINTER_MONITOR_WAITING && printer_monitor_heat_allowed(&m));
}
static void finish_seen_after_reconnect(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_RUNNING, 0);
  const printer_link_status_t down = link(false, false, PRINTER_RUN_UNKNOWN, 0);
  printer_monitor_step(&m, &down, 1000);
  step(&m, PRINTER_RUN_FINISHED, 600000);
  assert(m.phase == PRINTER_MONITOR_OFF && m.last_end == PRINTER_MONITOR_END_FINISHED);
}
static void unknown_state_holds(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_RUNNING, 0);
  printer_link_status_t l = link(true, false, PRINTER_RUN_UNKNOWN, 500);
  printer_monitor_step(&m, &l, 1000);
  assert(m.phase == PRINTER_MONITOR_WATCHING && m.hold == PRINTER_HOLD_UNKNOWN);
  l = link(true, true, PRINTER_RUN_UNKNOWN, 1500);
  printer_monitor_step(&m, &l, 2000);
  assert(m.hold == PRINTER_HOLD_UNKNOWN && !printer_monitor_heat_allowed(&m));
}
static void silence_rule(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_RUNNING, 1000);
  printer_link_status_t l = at(PRINTER_RUN_RUNNING, 1000);
  printer_monitor_step(&m, &l, 300999);
  assert(m.hold == PRINTER_HOLD_NONE);
  printer_monitor_step(&m, &l, 301000);
  assert(m.hold == PRINTER_HOLD_DISCONNECTED && m.phase == PRINTER_MONITOR_WATCHING);
  l.received_ms = 301500;
  printer_monitor_step(&m, &l, 302000);
  assert(m.hold == PRINTER_HOLD_NONE);

  m = started(PRINTER_RUN_PAUSED, 1000);
  l = at(PRINTER_RUN_PAUSED, 1000);
  printer_monitor_step(&m, &l, 900000);
  assert(m.hold == PRINTER_HOLD_PAUSED);
}
static void stop_reasons(void) {
  printer_monitor_status_t m = started(PRINTER_RUN_RUNNING, 0);
  printer_monitor_stop(&m, PRINTER_MONITOR_END_STOPPED);
  assert(m.phase == PRINTER_MONITOR_OFF && m.hold == PRINTER_HOLD_NONE);
  assert(m.last_end == PRINTER_MONITOR_END_STOPPED);
  step(&m, PRINTER_RUN_FINISHED, 1000);
  assert(m.last_end == PRINTER_MONITOR_END_STOPPED);
  assert(!printer_monitor_end_disables_heating(PRINTER_MONITOR_END_STOPPED));
  assert(!printer_monitor_end_disables_heating(PRINTER_MONITOR_END_HEATING_OFF));
  assert(!printer_monitor_end_disables_heating(PRINTER_MONITOR_END_NONE));
  assert(printer_monitor_end_disables_heating(PRINTER_MONITOR_END_SETTINGS));
  assert(printer_monitor_end_disables_heating(PRINTER_MONITOR_END_FAILED));
}

int main(void) {
  start_refused_when_disconnected_or_unknown();
  start_refused_when_already_on();
  start_states();
  leftover_finish_then_new_print();
  pause_holds_and_resumes();
  failed_from_paused_and_prepare();
  idle_after_active_ends();
  wait_timeout();
  disconnect_holds_then_recovers();
  finish_seen_after_reconnect();
  unknown_state_holds();
  silence_rule();
  stop_reasons();
  puts("printer_monitor: PASS");
  return 0;
}
