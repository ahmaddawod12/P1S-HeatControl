/* Monitor P1S coordinator: printer state → runtime heat permits and end
 * decisions, driven through the same poll the worker task runs. */
#include "printer_control.h"
#include "printer_control_engine.h"
#include "printer_monitor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t now;
static app_status_t heat;
static printer_link_status_t link_state;
static printer_monitor_phase_t runtime_phase;
static bool last_end_disable, stored_present, monitor_recoverable, clear_changed_record;
static unsigned begins, permits, ends, saves;
static uint64_t last_permit;
static uint64_t runtime_lock_delay;
static esp_err_t begin_result;
static esp_err_t clear_result;
static service_operation_t owner;
static printer_connection_config_t stored;

static void heat_snapshot(app_status_t *out, void *c) { (void)c; *out = heat; }
static esp_err_t monitor_begin(printer_monitor_phase_t phase, uint64_t until, uint64_t deadline, void *c) {
  (void)c;
  assert(owner == SERVICE_OPERATION_PRINTER_MONITOR);
  now += runtime_lock_delay;
  if (deadline && now >= deadline) return ESP_ERR_TIMEOUT;
  ++begins;
  last_permit = until;
  if (begin_result == ESP_OK) {
    runtime_phase = phase;
    monitor_recoverable = true;
  }
  return begin_result;
}
static esp_err_t monitor_permit(printer_monitor_phase_t phase, uint64_t until, void *c) {
  (void)c;
  if (runtime_phase == PRINTER_MONITOR_OFF)
    return ESP_ERR_INVALID_STATE;
  runtime_phase = phase;
  monitor_recoverable = true;
  ++permits;
  last_permit = until;
  return ESP_OK;
}
static esp_err_t suspend_recovery(void *c) {
  (void)c;
  monitor_recoverable = false;
  return ESP_OK;
}
static esp_err_t monitor_end(bool disable, uint64_t deadline,
    const struct app_runtime_enable_guard *guard, uint32_t revision, void *c) {
  (void)c;
  (void)guard;
  (void)revision;
  now += runtime_lock_delay;
  if (deadline && now >= deadline) return ESP_ERR_TIMEOUT;
  ++ends;
  last_end_disable = disable;
  runtime_phase = PRINTER_MONITOR_OFF;
  monitor_recoverable = false;
  if (disable)
    heat.enabled = false;
  return ESP_OK;
}
static printer_monitor_phase_t monitor_phase(void *c) { (void)c; return runtime_phase; }
static esp_err_t configure(const printer_connection_config_t *v, void *c) {
  (void)v; (void)c;
  return ESP_OK;
}
static void tick(uint64_t at, void *c) { (void)at; (void)c; }
static void link_status(printer_link_status_t *out, void *c) { (void)c; *out = link_state; }
static esp_err_t load(printer_connection_config_t *out, void *c) {
  (void)c;
  if (!stored_present)
    return ESP_ERR_NOT_FOUND;
  *out = stored;
  return ESP_OK;
}
static esp_err_t save(const printer_connection_config_t *v, void *c) {
  (void)c;
  assert(!monitor_recoverable);
  ++saves;
  stored = *v;
  stored_present = true;
  return ESP_OK;
}
static esp_err_t clear(void *c) {
  (void)c;
  assert(!monitor_recoverable);
  if (clear_result == ESP_OK || clear_changed_record) stored_present = false;
  return clear_result;
}
static bool gate_begin(service_operation_t op, void *c) {
  (void)c;
  if (owner != SERVICE_OPERATION_NONE)
    return false;
  owner = op;
  return true;
}
static void gate_end(service_operation_t op, void *c) {
  (void)c;
  if (owner == op)
    owner = SERVICE_OPERATION_NONE;
}
static uint64_t clock_now(void *c) { (void)c; return now; }

static const printer_control_ops_t ops = {
    .heat_snapshot = heat_snapshot, .monitor_begin = monitor_begin,
    .monitor_permit = monitor_permit, .monitor_suspend_recovery = suspend_recovery,
    .monitor_end = monitor_end,
    .monitor_phase = monitor_phase, .link_configure = configure,
    .link_tick = tick, .link_status = link_status, .load_config = load,
    .save_config = save, .clear_config = clear, .gate_begin = gate_begin,
    .gate_end = gate_end, .now_ms = clock_now};

static printer_control_status_t status_now(void) {
  printer_control_status_t s;
  printer_control_get_status(&s);
  return s;
}
static void restart(void) {
  assert(printer_control_start_with_ops(&ops) == ESP_OK);
  printer_control_poll();
}
static void reset(void) {
  now = 1000;
  runtime_lock_delay = 0;
  heat = app_status_initial();
  heat.enabled = true;
  heat.state = HEAT_STATE_HEATING;
  link_state = (printer_link_status_t){.connected = true, .state_known = true,
                                       .state = PRINTER_RUN_RUNNING,
                                       .received_ms = 1000};
  runtime_phase = PRINTER_MONITOR_OFF;
  monitor_recoverable = false;
  last_end_disable = false;
  begins = permits = ends = saves = 0;
  last_permit = 0;
  begin_result = ESP_OK;
  clear_result = ESP_OK;
  clear_changed_record = false;
  owner = SERVICE_OPERATION_NONE;
  stored = (printer_connection_config_t){.enabled = true, .ipv4 = "192.168.1.20",
                                         .serial = "SERIAL123",
                                         .access_code = "code"};
  stored_present = true;
  restart();
}
static void poll_at(printer_run_state_t state, uint64_t t) {
  now = t;
  link_state.state = state;
  link_state.received_ms = t;
  printer_control_poll();
}
static void expect_reason(const char *reason, bool can_monitor) {
  const printer_control_status_t s = status_now();
  if (strcmp(s.reason, reason)) {
    fprintf(stderr, "reason %s, wanted %s\n", s.reason, reason);
    assert(0);
  }
  assert(s.can_monitor == can_monitor);
}

static void control_reasons(void) {
  reset();
  expect_reason("ready", true);
  stored_present = false;
  restart();
  expect_reason("configuration_required", false);
  reset();
  stored.enabled = false;
  restart();
  expect_reason("printer_disabled", false);
  reset();
  link_state.connected = false;
  printer_control_poll();
  expect_reason("disconnected", false);
  reset();
  link_state.state_known = false;
  printer_control_poll();
  expect_reason("state_unknown", false);
  reset();
  heat.enabled = false;
  printer_control_poll();
  expect_reason("heating_off", false);
  reset();
  heat.state = HEAT_STATE_FAULT;
  heat.fault = HEAT_FAULT_OVERTEMP;
  printer_control_poll();
  expect_reason("heating_off", false);
  reset();
  assert(printer_control_monitor_start() == ESP_OK);
  expect_reason("monitoring", false);
  const printer_control_status_t s = status_now();
  assert(s.configured && s.enabled && s.link.connected);
  assert(s.link.state == PRINTER_RUN_RUNNING);
}
static void control_start_refused_when_not_ready(void) {
  reset();
  link_state.connected = false;
  printer_control_poll();
  assert(printer_control_monitor_start() == ESP_ERR_NOT_ALLOWED);
  heat.enabled = false;
  link_state.connected = true;
  printer_control_poll();
  assert(printer_control_monitor_start() == ESP_ERR_NOT_ALLOWED);
  assert(begins == 0 && status_now().monitor.phase == PRINTER_MONITOR_OFF);
}
static void control_begin_refused_by_runtime(void) {
  reset();
  begin_result = ESP_ERR_NOT_ALLOWED;
  assert(printer_control_monitor_start() == ESP_ERR_NOT_ALLOWED);
  assert(status_now().monitor.phase == PRINTER_MONITOR_OFF);
  begin_result = ESP_OK;
  assert(printer_control_monitor_start() == ESP_OK);
}
static void control_start_paused_begins_held(void) {
  reset();
  poll_at(PRINTER_RUN_PAUSED, 1000);
  assert(printer_control_monitor_start() == ESP_OK);
  assert(begins == 1 && last_permit == 0);
  assert(status_now().monitor.hold == PRINTER_HOLD_PAUSED);
}
static void control_start_refreshes_completion_baseline(void) {
  reset();
  /* A print ended after the last coordinator poll but before the button. */
  link_state.state = PRINTER_RUN_FINISHED;
  link_state.completed_prints = 1;
  link_state.last_completed_state = PRINTER_RUN_FINISHED;
  assert(printer_control_monitor_start() == ESP_OK);
  assert(status_now().monitor.phase == PRINTER_MONITOR_WAITING);
  printer_control_poll();
  assert(status_now().monitor.phase == PRINTER_MONITOR_WAITING);
  assert(ends == 0 && heat.enabled);
}
static void control_leftover_finish_sequence(void) {
  reset();
  poll_at(PRINTER_RUN_FINISHED, 1000);
  assert(printer_control_monitor_start() == ESP_OK);
  assert(last_permit == 6000);
  assert(status_now().monitor.phase == PRINTER_MONITOR_WAITING);
  poll_at(PRINTER_RUN_FINISHED, 2000);
  assert(last_permit == 7000 && ends == 0);
  poll_at(PRINTER_RUN_PREPARING, 3000);
  assert(last_permit == 8000);
  poll_at(PRINTER_RUN_RUNNING, 4000);
  assert(last_permit == 9000 && ends == 0);
  poll_at(PRINTER_RUN_FINISHED, 5000);
  assert(ends == 1 && last_end_disable);
  const printer_control_status_t s = status_now();
  assert(s.monitor.phase == PRINTER_MONITOR_OFF);
  assert(s.monitor.last_end == PRINTER_MONITOR_END_FINISHED);
  const unsigned permits_after_end = permits;
  poll_at(PRINTER_RUN_FINISHED, 6000);
  assert(permits == permits_after_end && ends == 1);
}
static void control_pause_permits_zero(void) {
  reset();
  assert(printer_control_monitor_start() == ESP_OK);
  poll_at(PRINTER_RUN_PAUSED, 2000);
  assert(last_permit == 0);
  poll_at(PRINTER_RUN_RUNNING, 3000);
  assert(last_permit == 8000);
}
static void control_stop_keeps_heating(void) {
  reset();
  assert(printer_control_monitor_start() == ESP_OK);
  assert(printer_control_monitor_stop() == ESP_OK);
  assert(ends == 1 && !last_end_disable);
  assert(status_now().monitor.last_end == PRINTER_MONITOR_END_STOPPED);
  assert(printer_control_monitor_stop() == ESP_OK);
  assert(ends == 1);
}
static void control_runtime_ended(void) {
  reset();
  assert(printer_control_monitor_start() == ESP_OK);
  runtime_phase = PRINTER_MONITOR_OFF; /* Disable, fault or maintenance in the runtime */
  heat.enabled = false;
  poll_at(PRINTER_RUN_RUNNING, 2000);
  assert(ends == 0);
  const printer_control_status_t s = status_now();
  assert(s.monitor.phase == PRINTER_MONITOR_OFF);
  assert(s.monitor.last_end == PRINTER_MONITOR_END_HEATING_OFF);
}
static void control_settings_change_ends(void) {
  reset();
  assert(printer_control_monitor_start() == ESP_OK);
  printer_config_request_t request = {.value = stored, .access_code_present = true};
  strcpy(request.value.ipv4, "192.168.1.21");
  assert(printer_control_request_config(&request) == ESP_OK);
  poll_at(PRINTER_RUN_RUNNING, 2000);
  assert(saves == 1 && ends == 1 && last_end_disable);
  assert(status_now().monitor.last_end == PRINTER_MONITOR_END_SETTINGS);
}
static void control_wait_remaining(void) {
  reset();
  poll_at(PRINTER_RUN_IDLE, 1000);
  assert(printer_control_monitor_start() == ESP_OK);
  poll_at(PRINTER_RUN_IDLE, 61000);
  assert(status_now().wait_remaining_ms == 3600000ULL - 60000ULL);
  poll_at(PRINTER_RUN_IDLE, 1000 + 3600000);
  assert(ends == 1 && last_end_disable);
  assert(status_now().monitor.last_end == PRINTER_MONITOR_END_NO_PRINT);
}

static void control_reboot_ends_a_watched_print(void) {
  reset();
  runtime_phase = PRINTER_MONITOR_WATCHING;
  link_state = (printer_link_status_t){0};
  restart();
  link_state.connected = link_state.state_known = true;
  /* This active report could belong to a later job. */
  poll_at(PRINTER_RUN_RUNNING, 2000);
  assert(status_now().configured && status_now().monitor.phase == PRINTER_MONITOR_OFF);
  assert(status_now().monitor.last_end == PRINTER_MONITOR_END_HEATING_OFF);
  assert(ends == 1 && last_end_disable && !heat.enabled && permits == 0);
}

static void control_reboot_ends_a_waiting_session(void) {
  reset();
  runtime_phase = PRINTER_MONITOR_WAITING;
  link_state = (printer_link_status_t){0};
  restart();
  assert(status_now().monitor.phase == PRINTER_MONITOR_OFF && last_permit == 0);
  link_state.connected = link_state.state_known = true;
  poll_at(PRINTER_RUN_FINISHED, 2000);
  poll_at(PRINTER_RUN_PAUSED, 3000);
  poll_at(PRINTER_RUN_RUNNING, 4000);
  assert(runtime_phase == PRINTER_MONITOR_OFF && !heat.enabled && begins == 0 && permits == 0);
  assert(ends == 1 && last_end_disable && status_now().wait_remaining_ms == 0);
}

static void control_reboot_ends_monitoring_without_a_saved_printer(void) {
  reset();
  runtime_phase = PRINTER_MONITOR_WAITING;
  stored_present = false;
  restart();
  assert(ends == 1 && last_end_disable && begins == 0 && permits == 0);
  assert(status_now().monitor.last_end == PRINTER_MONITOR_END_HEATING_OFF);
}

static void control_forget_reports_async_completion(void) {
  reset();
  assert(printer_control_monitor_start() == ESP_OK);
  assert(printer_control_forget_config() == ESP_OK);
  assert(stored_present && status_now().configured && status_now().applying);
  assert(runtime_phase != PRINTER_MONITOR_OFF && owner == SERVICE_OPERATION_PRINTER_CONFIG);
  printer_control_poll();
  assert(!stored_present && !status_now().configured && !status_now().applying);
  assert(!strcmp(status_now().config_result, "saved"));
  assert(runtime_phase == PRINTER_MONITOR_OFF && !heat.enabled && owner == SERVICE_OPERATION_NONE);

  reset();
  assert(printer_control_monitor_start() == ESP_OK);
  clear_result = ESP_FAIL;
  assert(printer_control_forget_config() == ESP_OK);
  assert(status_now().applying);
  printer_control_poll();
  assert(stored_present && status_now().configured && !status_now().applying);
  assert(!strcmp(status_now().config_result, "save_failed"));
  assert(runtime_phase == PRINTER_MONITOR_WATCHING && heat.enabled && monitor_recoverable);
  assert(owner == SERVICE_OPERATION_NONE);

  reset();
  assert(printer_control_monitor_start() == ESP_OK);
  clear_result = ESP_FAIL;
  clear_changed_record = true;
  assert(printer_control_forget_config() == ESP_OK);
  printer_control_poll();
  assert(!strcmp(status_now().config_result, "save_failed"));
  assert(!stored_present && !monitor_recoverable && !heat.enabled);
  assert(status_now().monitor.last_end == PRINTER_MONITOR_END_SETTINGS);
}

static void control_start_respects_service_gate(void) {
  reset();
  owner = SERVICE_OPERATION_OTA;
  assert(printer_control_monitor_start() == ESP_ERR_NOT_ALLOWED);
  assert(runtime_phase == PRINTER_MONITOR_OFF && begins == 0 && owner == SERVICE_OPERATION_OTA);
  owner = SERVICE_OPERATION_NONE;
  assert(printer_control_monitor_start() == ESP_OK);
  assert(runtime_phase != PRINTER_MONITOR_OFF && owner == SERVICE_OPERATION_NONE);
  printer_control_monitor_stop();
  begin_result = ESP_ERR_NOT_ALLOWED;
  assert(printer_control_monitor_start() == ESP_ERR_NOT_ALLOWED);
  assert(owner == SERVICE_OPERATION_NONE);
}

static void remote_deadlines_preserve_the_current_monitor_phase(void) {
  reset();
  poll_at(PRINTER_RUN_PAUSED, 1000);
  runtime_lock_delay = 100;
  assert(printer_control_monitor_start_until(now + 25) == ESP_ERR_TIMEOUT);
  assert(status_now().monitor.phase == PRINTER_MONITOR_OFF && begins == 0);
  assert(owner == SERVICE_OPERATION_NONE);

  assert(printer_control_monitor_start_until(now + 200) == ESP_OK);
  assert(status_now().monitor.hold == PRINTER_HOLD_PAUSED);
  assert(printer_control_monitor_stop_until(now + 25, NULL, 0) == ESP_ERR_TIMEOUT);
  assert(status_now().monitor.phase == PRINTER_MONITOR_WATCHING && ends == 0);
  assert(status_now().monitor.hold == PRINTER_HOLD_PAUSED);
  assert(printer_control_monitor_stop_until(now + 200, NULL, 0) == ESP_OK);
  assert(status_now().monitor.last_end == PRINTER_MONITOR_END_STOPPED);
}

int main(void) {
  control_forget_reports_async_completion();
  control_start_respects_service_gate();
  remote_deadlines_preserve_the_current_monitor_phase();
  control_reasons();
  control_start_refused_when_not_ready();
  control_begin_refused_by_runtime();
  control_start_paused_begins_held();
  control_start_refreshes_completion_baseline();
  control_leftover_finish_sequence();
  control_pause_permits_zero();
  control_stop_keeps_heating();
  control_runtime_ended();
  control_settings_change_ends();
  control_wait_remaining();
  control_reboot_ends_a_watched_print();
  control_reboot_ends_a_waiting_session();
  control_reboot_ends_monitoring_without_a_saved_printer();
  puts("printer_control: PASS");
  return 0;
}
