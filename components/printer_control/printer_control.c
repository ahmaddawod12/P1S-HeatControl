#include "printer_control.h"
#include "device_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "printer_control_engine.h"
#include "printer_cycle.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static struct {
  printer_control_ops_t ops;
  printer_connection_config_t config, pending;
  printer_control_status_t status;
  printer_cycle_t cycle;
  SemaphoreHandle_t lock, completion;
  TaskHandle_t task;
  uint64_t next_id, last_renew;
  unsigned command;
  unsigned token_stop_generation;
  esp_err_t completion_result;
  bool configured, cycle_owner;
} s;
static atomic_bool started, stopped;
static atomic_uint stop_generation;
static bool lock(void) {
  return xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) == pdTRUE;
}
static void unlock(void) { xSemaphoreGive(s.lock); }
static bool active(printer_cycle_state_t state) {
  return state == PRINTER_CYCLE_PREHEATING ||
         state == PRINTER_CYCLE_RESUME_PENDING ||
         state == PRINTER_CYCLE_PRINTING;
}
static void input(printer_cycle_input_t *in,
                  const printer_observation_t *observation) {
  memset(in, 0, sizeof(*in));
  in->printer = *observation;
  in->now_ms = s.ops.now_ms(s.ops.ctx);
  in->qualified = s.ops.resume_qualified;
  s.ops.heat_snapshot(&in->heat, s.ops.ctx);
  s.ops.auto_snapshot(&in->automatic, s.ops.ctx);
}
static uint64_t expiry(const printer_cycle_input_t *in, uint64_t deadline) {
  uint64_t until = in->now_ms + 5000;
  if (until > in->printer.received_ms + PRINTER_TELEMETRY_MS)
    until = in->printer.received_ms + PRINTER_TELEMETRY_MS;
  if (until > deadline)
    until = deadline;
  return until;
}
static void redacted_config(void) {
  s.status.configured = s.configured;
  s.status.enabled = s.configured && s.config.enabled;
  s.status.access_code_set = s.configured && s.config.access_code[0];
  strcpy(s.status.ipv4, s.config.ipv4);
  strcpy(s.status.serial, s.config.serial);
  strcpy(s.status.ntp_host, s.config.ntp_host);
}
static void publish_cycle(void) {
  printer_cycle_snapshot(&s.cycle, &s.status.cycle);
  s.status.can_arm =
      s.cycle.snapshot.arm_token[0] && !s.status.applying && !s.cycle_owner;
  const char *reason = !s.ops.resume_qualified ? "qualification_required"
                       : !s.configured         ? "configuration_required"
                       : s.status.applying     ? "configuration_pending"
                       : s.cycle_owner         ? "cycle_active"
                       : s.status.can_arm      ? "ready"
                                               : s.status.observation.reason;
  snprintf(s.status.reason, sizeof(s.status.reason), "%s", reason);
}
#ifndef PRINTER_CONTROL_MANUAL_POLL
static void worker(void *ctx) {
  (void)ctx;
  while (true) {
    printer_control_poll();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
#endif
esp_err_t printer_control_start_with_ops(const printer_control_ops_t *ops) {
  if (!ops || !ops->heat_acquire || !ops->heat_renew || !ops->heat_revoke ||
      !ops->heat_snapshot || !ops->auto_snapshot || !ops->link_configure ||
      !ops->link_tick || !ops->link_snapshot || !ops->link_resume ||
      !ops->load_config || !ops->save_config || !ops->clear_config ||
      !ops->gate_begin || !ops->gate_end || !ops->now_ms || !ops->random_bytes)
    return ESP_ERR_INVALID_ARG;
#ifndef PRINTER_CONTROL_MANUAL_POLL
  if (atomic_load(&started))
    return ESP_OK;
#endif
  if (!s.lock)
    s.lock = xSemaphoreCreateMutex();
  if (!s.completion)
    s.completion = xSemaphoreCreateBinary();
  if (!s.lock || !s.completion)
    return ESP_ERR_NO_MEM;
  s.ops = *ops;
  memset(&s.status, 0, sizeof(s.status));
  memset(&s.config, 0, sizeof(s.config));
  s.command = 0;
  s.cycle_owner = false;
  s.last_renew = 0;
  printer_cycle_init(&s.cycle);
  uint32_t seed = 0;
  ops->random_bytes(&seed, sizeof(seed), ops->ctx);
  s.next_id = ((uint64_t)seed << 32) | 1;
  s.configured = ops->load_config(&s.config, ops->ctx) == ESP_OK;
  if (s.configured)
    (void)ops->link_configure(&s.config, ops->ctx);
  else
    memset(&s.config, 0, sizeof(s.config));
  redacted_config();
  publish_cycle();
  atomic_store(&stopped, false);
  atomic_store(&stop_generation, 0);
  s.token_stop_generation = 0;
  atomic_store(&started, true);
#ifndef PRINTER_CONTROL_MANUAL_POLL
  if (xTaskCreate(worker, "printer_control", 8192, NULL, 3, &s.task) !=
      pdPASS) {
    atomic_store(&started, false);
    return ESP_ERR_NO_MEM;
  }
#endif
  return ESP_OK;
}
esp_err_t printer_control_arm(const printer_arm_request_t *request) {
  if (!atomic_load(&started))
    return ESP_ERR_INVALID_STATE;
  if (!s.ops.gate_begin(SERVICE_OPERATION_PRINTER_CYCLE, s.ops.ctx))
    return ESP_ERR_NOT_ALLOWED;
  if (!lock()) {
    s.ops.gate_end(SERVICE_OPERATION_PRINTER_CYCLE, s.ops.ctx);
    return ESP_ERR_TIMEOUT;
  }
  printer_cycle_input_t in;
  input(&in, &s.status.observation);
  /* arm() changes only these fields; avoid copying both full job keys onto the
   * HTTP stack just to roll back a rejected runtime acquisition. */
  printer_cycle_snapshot_t saved = s.cycle.snapshot;
  printer_safety_history_t saved_history = s.cycle.armed_history;
  bool saved_reached = s.cycle.reached_target,
       saved_settling = s.cycle.settling;
  bool acquired = false;
  esp_err_t result =
      s.token_stop_generation != atomic_load(&stop_generation) ||
              (s.ops.link_live &&
               !s.ops.link_live(in.printer.connection_epoch, s.ops.ctx))
          ? ESP_ERR_NOT_ALLOWED
          : printer_cycle_arm(&s.cycle, request, &in, ++s.next_id);
  if (result == ESP_OK) {
    atomic_store(&stopped, false);
    result = s.ops.heat_acquire(
        s.cycle.snapshot.cycle_id, s.cycle.snapshot.target_c,
        expiry(&in, s.cycle.snapshot.deadline_ms), s.ops.ctx);
    acquired = result == ESP_OK;
    if (result == ESP_OK &&
        (atomic_load(&stopped) ||
         s.token_stop_generation != atomic_load(&stop_generation))) {
      s.ops.heat_revoke(0, s.ops.ctx);
      result = ESP_ERR_NOT_ALLOWED;
    }
  }
  if (result == ESP_OK) {
    s.cycle_owner = true;
    s.last_renew = in.now_ms;
  } else if (acquired) {
    printer_cycle_stop(&s.cycle, PRINTER_OUTCOME_OPERATOR_STOP, in.now_ms);
    s.cycle_owner = true;
  } else {
    s.cycle.snapshot = saved;
    s.cycle.armed_history = saved_history;
    s.cycle.reached_target = saved_reached;
    s.cycle.settling = saved_settling;
    s.ops.gate_end(SERVICE_OPERATION_PRINTER_CYCLE, s.ops.ctx);
  }
  publish_cycle();
  unlock();
  return result;
}
esp_err_t printer_control_stop(void) {
  if (!atomic_load(&started))
    return ESP_ERR_INVALID_STATE;
  atomic_store(&stopped, true);
  atomic_fetch_add(&stop_generation, 1);
  s.ops.heat_revoke(0, s.ops.ctx);
  if (s.ops.link_cancel)
    s.ops.link_cancel(s.ops.ctx);
  return ESP_OK;
}
static esp_err_t config_admit(const printer_config_request_t *request,
                              bool forget) {
  if (!atomic_load(&started))
    return ESP_ERR_INVALID_STATE;
  if (!forget && !request)
    return ESP_ERR_INVALID_ARG;
  if (!s.ops.gate_begin(SERVICE_OPERATION_PRINTER_CONFIG, s.ops.ctx))
    return ESP_ERR_NOT_ALLOWED;
  if (!lock()) {
    s.ops.gate_end(SERVICE_OPERATION_PRINTER_CONFIG, s.ops.ctx);
    return ESP_ERR_TIMEOUT;
  }
  printer_connection_config_t candidate = {0};
  esp_err_t result = ESP_OK;
  if (!forget) {
    candidate = request->value;
    if (!request->access_code_present && s.configured &&
        !strcmp(candidate.serial, s.config.serial))
      strcpy(candidate.access_code, s.config.access_code);
    if (!printer_config_valid(&candidate))
      result = ESP_ERR_INVALID_ARG;
  }
  if (s.command)
    result = ESP_ERR_NOT_ALLOWED;
  if (result == ESP_OK) {
    (void)xSemaphoreTake(s.completion, 0);
    s.pending = candidate;
    s.command = forget ? 2 : 1;
    s.status.applying = true;
    s.status.can_arm = false;
    s.status.cycle.arm_token[0] = 0;
    strcpy(s.status.config_result, "pending");
  }
  unlock();
  if (result != ESP_OK)
    s.ops.gate_end(SERVICE_OPERATION_PRINTER_CONFIG, s.ops.ctx);
  return result;
}
esp_err_t
printer_control_request_config(const printer_config_request_t *request) {
  return config_admit(request, false);
}
esp_err_t printer_control_forget_config(void) {
  esp_err_t result = config_admit(NULL, true);
  if (result != ESP_OK)
    return result;
#ifdef PRINTER_CONTROL_MANUAL_POLL
  printer_control_poll();
#endif
  if (xSemaphoreTake(s.completion, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  return s.completion_result;
}
void printer_control_poll(void) {
  if (!atomic_load(&started))
    return;
  unsigned command = 0;
  printer_connection_config_t pending = {0};
  if (lock()) {
    command = s.command;
    s.command = 0;
    pending = s.pending;
    unlock();
  }
  if (command) {
    esp_err_t result = command == 1 ? s.ops.save_config(&pending, s.ops.ctx)
                                    : s.ops.clear_config(s.ops.ctx);
    /* Keep the working link until persistence has succeeded. */
    if (result == ESP_OK)
      (void)s.ops.link_configure(&pending, s.ops.ctx);
    if (lock()) {
      if (result == ESP_OK) {
        s.config = pending;
        s.configured = command == 1;
        redacted_config();
        s.cycle.snapshot.arm_token[0] = 0;
      }
      s.status.applying = false;
      strcpy(s.status.config_result,
             result == ESP_OK ? "saved" : "save_failed");
      s.completion_result = result;
      publish_cycle();
      unlock();
    }
    s.ops.gate_end(SERVICE_OPERATION_PRINTER_CONFIG, s.ops.ctx);
    if (command == 2)
      xSemaphoreGive(s.completion);
  }
  s.ops.link_tick(s.ops.now_ms(s.ops.ctx), s.ops.ctx);
  printer_observation_t observation;
  s.ops.link_snapshot(&observation, s.ops.ctx);
  printer_cycle_input_t in;
  printer_cycle_effects_t effects = {0};
  uint64_t id = 0;
  bool release = false;
  if (!lock())
    return;
  /* Arm admission uses this lock too; never apply an earlier lease snapshot
   * to a cycle admitted while the poll was collecting telemetry. */
  input(&in, &observation);
  s.status.observation = observation;
  if (atomic_load(&stopped) && active(s.cycle.snapshot.state))
    printer_cycle_stop(&s.cycle, PRINTER_OUTCOME_OPERATOR_STOP, in.now_ms);
  printer_cycle_step(&s.cycle, &in, &effects);
  id = s.cycle.snapshot.cycle_id;
  char token[33];
  unsigned char bytes[16];
  s.ops.random_bytes(bytes, sizeof(bytes), s.ops.ctx);
  for (unsigned i = 0; i < 16; ++i)
    snprintf(token + i * 2, 3, "%02x", bytes[i]);
  if (s.token_stop_generation != atomic_load(&stop_generation)) {
    s.cycle.snapshot.arm_token[0] = 0;
    s.token_stop_generation = atomic_load(&stop_generation);
  }
  if (!s.cycle_owner && !s.status.applying && s.configured && s.config.enabled)
    printer_cycle_offer_arm(&s.cycle, &in, token);
  if (s.cycle_owner && !active(s.cycle.snapshot.state) &&
      !in.automatic.active && in.automatic.teardown_applied) {
    s.cycle_owner = false;
    release = true;
  }
  printer_cycle_snapshot_t dispatch = s.cycle.snapshot;
  printer_safety_history_t dispatch_history = s.cycle.armed_history;
  publish_cycle();
  unlock();
  if (effects.revoke_heat)
    s.ops.heat_revoke(id, s.ops.ctx);
  if (release)
    s.ops.gate_end(SERVICE_OPERATION_PRINTER_CYCLE, s.ops.ctx);
  if (effects.renew_heat && in.now_ms - s.last_renew >= 1000 &&
      !atomic_load(&stopped)) {
    esp_err_t result =
        s.ops.heat_renew(id,
                         expiry(&in, dispatch.state == PRINTER_CYCLE_PRINTING
                                         ? UINT64_MAX
                                         : dispatch.deadline_ms),
                         s.ops.ctx);
    s.last_renew = in.now_ms;
    if (result != ESP_OK) {
      s.ops.heat_revoke(id, s.ops.ctx);
      if (lock()) {
        printer_cycle_stop(&s.cycle, PRINTER_OUTCOME_LEASE_EXPIRED, in.now_ms);
        publish_cycle();
        unlock();
      }
    }
  }
  if (effects.send_resume) {
    /* Refresh both safety authorities after all policy work, before consuming
     * transport permission. */
    s.ops.link_tick(s.ops.now_ms(s.ops.ctx), s.ops.ctx);
    s.ops.link_snapshot(&observation, s.ops.ctx);
    input(&in, &observation);
    bool allowed =
        !atomic_load(&stopped) && in.automatic.active &&
        in.automatic.cycle_id == id && in.automatic.expires_ms > in.now_ms &&
        printer_cycle_dispatch_ready(&in.heat, dispatch.target_c, in.now_ms);
    printer_dispatch_result_t result =
        allowed
            ? s.ops.link_resume(id, &dispatch.job, &dispatch_history,
                                dispatch.connection_epoch, in.now_ms + 1000,
                                s.ops.ctx)
            : PRINTER_DISPATCH_REJECTED;
    if (lock()) {
      printer_cycle_note_dispatch(&s.cycle, result, s.ops.now_ms(s.ops.ctx));
      publish_cycle();
      unlock();
    }
    if (result != PRINTER_DISPATCH_SENT || atomic_load(&stopped))
      s.ops.heat_revoke(id, s.ops.ctx);
  }
}
void printer_control_get_status(printer_control_status_t *out) {
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  if (!atomic_load(&started) || !lock()) {
    strcpy(out->reason, "unavailable");
    return;
  }
  *out = s.status;
  if (s.token_stop_generation != atomic_load(&stop_generation)) {
    out->can_arm = false;
    out->cycle.arm_token[0] = 0;
  }
  unlock();
  runtime_auto_status_t automatic;
  s.ops.auto_snapshot(&automatic, s.ops.ctx);
  if (active(out->cycle.state) &&
      (!automatic.active || automatic.cycle_id != out->cycle.cycle_id ||
       automatic.expires_ms <= s.ops.now_ms(s.ops.ctx) ||
       atomic_load(&stopped))) {
    out->cycle.state =
        atomic_load(&stopped) ? PRINTER_CYCLE_STOPPED : PRINTER_CYCLE_ERROR;
    out->cycle.outcome = atomic_load(&stopped) ? PRINTER_OUTCOME_OPERATOR_STOP
                                               : PRINTER_OUTCOME_LEASE_EXPIRED;
    out->can_arm = false;
    out->cycle.arm_token[0] = 0;
    strcpy(out->reason,
           atomic_load(&stopped) ? "operator_stop" : "lease_expired");
  }
}
