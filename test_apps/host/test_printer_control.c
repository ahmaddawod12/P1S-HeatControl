#include "printer_control.h"
#include "printer_control_engine.h"
#include "runtime_auto_lease.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t now = 1000;
static app_status_t heat;
static runtime_auto_lease_t lease;
static printer_observation_t observation;
static service_operation_t owner;
static unsigned resume_publish_count, revokes, configures, saves;
static bool fail_acquire, fail_renew, fail_save;
static bool drop_before_dispatch;
static unsigned dispatch_heat_reads;
static bool link_live_good = true;
static bool stop_during_acquire;
static bool schedule_arm, arm_before_policy_lock;
static printer_arm_request_t overlapping_arm;
BaseType_t __real_xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
BaseType_t __wrap_xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks) {
  if (arm_before_policy_lock) {
    arm_before_policy_lock = false;
    /* Schedule the HTTP task just before the poll acquires its policy lock. */
    assert(printer_control_arm(&overlapping_arm) == ESP_OK);
  }
  return __real_xSemaphoreTake(semaphore, ticks);
}
static bool live(uint32_t epoch, void *ctx) {
  (void)ctx;
  return link_live_good && epoch == 7;
}
static esp_err_t acquire(uint64_t id, float target, uint64_t expiry, void *c) {
  (void)c;
  assert(target == 60);
  if (fail_acquire)
    return ESP_ERR_NOT_ALLOWED;
  esp_err_t e = runtime_auto_lease_acquire(&lease, id, now, expiry);
  if (e == ESP_OK)
    heat.enabled = true;
  if (e == ESP_OK && stop_during_acquire)
    assert(printer_control_stop() == ESP_OK);
  return e;
}
static esp_err_t renew(uint64_t id, uint64_t expiry, void *c) {
  (void)c;
  return fail_renew ? ESP_FAIL
                    : runtime_auto_lease_renew(&lease, id, now, expiry);
}
static void revoke(uint64_t id, void *c) {
  (void)c;
  ++revokes;
  runtime_auto_lease_revoke(&lease, id, RUNTIME_AUTO_OPERATOR_STOP);
  heat.enabled = false;
  lease.status.teardown_applied = true;
}
static void heat_snapshot(app_status_t *out, void *c) {
  (void)c;
  *out = heat;
  if (drop_before_dispatch && ++dispatch_heat_reads == 2)
    out->temperature.raw_c = out->temperature.filtered_c = 58;
}
static void auto_snapshot(runtime_auto_status_t *out, void *c) {
  (void)c;
  runtime_auto_lease_step(&lease, now);
  if (!lease.status.active) {
    heat.enabled = false;
    lease.status.teardown_applied = true;
  }
  *out = lease.status;
}
static esp_err_t configure(const printer_connection_config_t *v, void *c) {
  (void)v;
  (void)c;
  ++configures;
  return ESP_OK;
}
static void tick(uint64_t at, void *c) {
  (void)at;
  (void)c;
}
static void snapshot(printer_observation_t *out, void *c) {
  (void)c;
  *out = observation;
  if (schedule_arm) {
    schedule_arm = false;
    arm_before_policy_lock = true;
  }
}
static printer_dispatch_result_t resume(uint64_t id,
                                        const printer_job_key_t *job,
                                        const printer_safety_history_t *history,
                                        uint32_t epoch, uint64_t deadline,
                                        void *c) {
  (void)c;
  assert(id && job->identity_valid && history && epoch == 7 &&
         deadline <= now + 1000);
  ++resume_publish_count;
  return PRINTER_DISPATCH_SENT;
}
static esp_err_t load(printer_connection_config_t *out, void *c) {
  (void)c;
  *out = (printer_connection_config_t){.enabled = true,
                                       .ipv4 = "192.168.1.20",
                                       .serial = "SERIAL123",
                                       .access_code = "Secret",
                                       .ntp_host = "lan.example"};
  return ESP_OK;
}
static esp_err_t save(const printer_connection_config_t *v, void *c) {
  (void)c;
  assert(!strcmp(v->access_code, "Secret"));
  ++saves;
  return fail_save ? ESP_FAIL : ESP_OK;
}
static esp_err_t clear(void *c) {
  (void)c;
  return fail_save ? ESP_FAIL : ESP_OK;
}
static bool gate_begin(service_operation_t v, void *c) {
  (void)c;
  if (owner)
    return false;
  owner = v;
  return true;
}
static void gate_end(service_operation_t v, void *c) {
  (void)c;
  assert(owner == v);
  owner = SERVICE_OPERATION_NONE;
}
static uint64_t clock_now(void *c) {
  (void)c;
  return now;
}
static void random_bytes(void *out, size_t n, void *c) {
  (void)c;
  memset(out, 0x34, n);
}
static printer_control_ops_t ops;
static void setup(bool qualified) {
  heat = app_status_initial();
  heat.target_c = 60;
  heat.temperature = (temperature_sample_t){.raw_c = 60,
                                            .filtered_c = 60,
                                            .valid = true,
                                            .consecutive_valid = 3,
                                            .timestamp_ms = now};
  runtime_auto_lease_init(&lease);
  owner = 0;
  resume_publish_count = revokes = 0;
  observation = (printer_observation_t){.state = PRINTER_RUN_PAUSED,
                                        .connection_epoch = 7,
                                        .received_ms = now,
                                        .connected = true,
                                        .fresh = true,
                                        .baseline_valid = true,
                                        .startup_eligible = true,
                                        .job = {.connection_epoch = 7,
                                                .print_epoch = 1,
                                                .start_time = 100,
                                                .identity_valid = true,
                                                .file = "job.3mf"}};
  ops = (printer_control_ops_t){.heat_acquire = acquire,
                                .heat_renew = renew,
                                .heat_revoke = revoke,
                                .heat_snapshot = heat_snapshot,
                                .auto_snapshot = auto_snapshot,
                                .link_configure = configure,
                                .link_tick = tick,
                                .link_snapshot = snapshot,
                                .link_resume = resume,
                                .link_live = live,
                                .load_config = load,
                                .save_config = save,
                                .clear_config = clear,
                                .gate_begin = gate_begin,
                                .gate_end = gate_end,
                                .now_ms = clock_now,
                                .random_bytes = random_bytes,
                                .resume_qualified = qualified};
  assert(printer_control_start_with_ops(&ops) == ESP_OK);
  printer_control_poll();
}
static printer_arm_request_t arm_request(void) {
  printer_control_status_t s;
  printer_control_get_status(&s);
  return (printer_arm_request_t){.initial_pause_confirmed = true,
                                 .arm_token = {0}};
}
static printer_arm_request_t offered(void) {
  printer_control_status_t s;
  printer_control_get_status(&s);
  printer_arm_request_t r = arm_request();
  strcpy(r.arm_token, s.cycle.arm_token);
  return r;
}
static void advance(uint64_t ms) {
  now += ms;
  heat.temperature.timestamp_ms = observation.received_ms = now;
  printer_control_poll();
}
static void test_duplicate_arm_has_one_attempt(void) {
  setup(true);
  printer_arm_request_t r = offered();
  assert(strlen(r.arm_token) == 32);
  assert(printer_control_arm(&r) == ESP_OK);
  assert(printer_control_arm(&r) == ESP_ERR_NOT_ALLOWED);
  advance(0);
  for (int i = 0; i < 30; ++i)
    advance(1000);
  assert(resume_publish_count == 1);
  advance(1000);
  assert(resume_publish_count == 1);
  observation.state = PRINTER_RUN_RUNNING;
  advance(100);
  /* A long print is not limited by the earlier Resume confirmation deadline. */
  for (int i = 0; i < 60; ++i)
    advance(1000);
  assert(lease.status.active && resume_publish_count == 1);
  observation.state = PRINTER_RUN_FINISHED;
  advance(100);
  advance(100);
  assert(!lease.status.active && lease.status.teardown_applied && owner == 0);
  assert(printer_control_arm(&r) == ESP_ERR_NOT_ALLOWED);
}
static void test_stalled_coordinator_loses_heat_ownership(void) {
  setup(true);
  printer_arm_request_t r = offered();
  assert(printer_control_arm(&r) == ESP_OK);
  advance(0);
  now += 5001;
  printer_control_status_t s;
  printer_control_get_status(&s);
  assert(!lease.status.active && lease.status.teardown_applied);
  assert(s.cycle.state == PRINTER_CYCLE_ERROR &&
         s.cycle.outcome == PRINTER_OUTCOME_LEASE_EXPIRED);
  assert(printer_control_stop() == ESP_OK);
  advance(0);
  assert(owner == 0 && resume_publish_count == 0);
}
static void test_stop_invalidates_a_delayed_arm_request(void) {
  setup(true);
  printer_arm_request_t r = offered();
  assert(printer_control_stop() == ESP_OK);
  printer_control_status_t status;
  printer_control_get_status(&status);
  assert(!status.can_arm && !status.cycle.arm_token[0]);
  assert(printer_control_arm(&r) == ESP_ERR_NOT_ALLOWED && owner == 0 &&
         !lease.status.active);
  setup(true);
  r = offered();
  stop_during_acquire = true;
  assert(printer_control_arm(&r) == ESP_ERR_NOT_ALLOWED &&
         !lease.status.active);
  assert(owner == SERVICE_OPERATION_PRINTER_CYCLE);
  stop_during_acquire = false;
  advance(0);
  assert(owner == 0);
}
static void test_qualification_and_failed_admissions(void) {
  setup(false);
  printer_arm_request_t r = offered();
  assert(printer_control_arm(&r) == ESP_ERR_NOT_ALLOWED);
  printer_control_status_t s;
  printer_control_get_status(&s);
  assert(!strcmp(s.reason, "qualification_required"));
  setup(true);
  r = offered();
  fail_acquire = true;
  assert(printer_control_arm(&r) != ESP_OK);
  assert(owner == 0);
  fail_acquire = false;
  link_live_good = false;
  assert(printer_control_arm(&r) == ESP_ERR_NOT_ALLOWED && owner == 0);
  link_live_good = true;
  printer_config_request_t request = {.value = {.enabled = true,
                                                .ipv4 = "192.168.1.21",
                                                .serial = "SERIAL123",
                                                .ntp_host = "lan.example"}};
  unsigned before = configures;
  strcpy(request.value.serial, "OTHER123");
  assert(printer_control_request_config(&request) == ESP_ERR_INVALID_ARG &&
         owner == 0 && configures == before);
  strcpy(request.value.serial, "SERIAL123");
  fail_save = true;
  assert(printer_control_request_config(&request) == ESP_OK);
  printer_control_poll();
  assert(owner == 0 && configures == before);
  assert(printer_control_forget_config() == ESP_FAIL);
  assert(configures == before);
  fail_save = false;
  assert(printer_control_request_config(&request) == ESP_OK);
  printer_control_poll();
  assert(configures == before + 1);
  assert(printer_control_forget_config() == ESP_OK);
  assert(configures == before + 2);
  setup(true);
  r = offered();
  assert(printer_control_arm(&r) == ESP_OK);
  fail_renew = true;
  advance(1000);
  advance(100);
  assert(owner == 0 && !lease.status.active);
  fail_renew = false;
}
static void test_readiness_is_rechecked_after_policy(void) {
  setup(true);
  printer_arm_request_t r = offered();
  assert(printer_control_arm(&r) == ESP_OK);
  advance(0);
  for (unsigned i = 0; i < 29; ++i)
    advance(1000);
  drop_before_dispatch = true;
  dispatch_heat_reads = 0;
  advance(1000);
  drop_before_dispatch = false;
  assert(dispatch_heat_reads == 2 && resume_publish_count == 0);
  assert(!lease.status.active);
  advance(1000);
  assert(owner == 0 && resume_publish_count == 0);
}
static void test_arm_during_poll_keeps_heat_ownership(void) {
  setup(true);
  overlapping_arm = offered();
  schedule_arm = true;
  printer_control_poll();
  assert(!schedule_arm && !arm_before_policy_lock);
  printer_control_status_t status;
  printer_control_get_status(&status);
  assert(status.cycle.state == PRINTER_CYCLE_PREHEATING &&
         status.cycle.outcome == PRINTER_OUTCOME_NONE);
  assert(lease.status.active && owner == SERVICE_OPERATION_PRINTER_CYCLE &&
         revokes == 0 && resume_publish_count == 0);
  advance(1000);
  assert(lease.status.active && owner == SERVICE_OPERATION_PRINTER_CYCLE);
}
int main(void) {
  test_arm_during_poll_keeps_heat_ownership();
  test_readiness_is_rechecked_after_policy();
  test_duplicate_arm_has_one_attempt();
  test_stalled_coordinator_loses_heat_ownership();
  test_stop_invalidates_a_delayed_arm_request();
  test_qualification_and_failed_admissions();
  puts("printer control: ok");
  return 0;
}
