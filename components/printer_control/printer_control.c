#include "printer_control.h"
#include "device_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "printer_control_engine.h"
#include "printer_monitor.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static struct {
  printer_control_ops_t ops;
  printer_connection_config_t config, pending;
  printer_control_status_t status;
  printer_monitor_status_t monitor;
  SemaphoreHandle_t lock, completion;
  TaskHandle_t task;
  unsigned command;
  esp_err_t completion_result;
  bool configured;
} s;
static atomic_bool started;
static bool lock(void) {
  return xSemaphoreTake(s.lock, pdMS_TO_TICKS(100)) == pdTRUE;
}
static void unlock(void) { xSemaphoreGive(s.lock); }
static void redacted_config(void) {
  s.status.configured = s.configured;
  s.status.enabled = s.configured && s.config.enabled;
  s.status.access_code_set = s.configured && s.config.access_code[0];
  strcpy(s.status.ipv4, s.config.ipv4);
  strcpy(s.status.serial, s.config.serial);
}
static const char *reason_for(const app_status_t *heat) {
  const printer_link_status_t *link = &s.status.link;
  if (!s.configured)
    return "configuration_required";
  if (!s.config.enabled)
    return "printer_disabled";
  if (s.status.applying)
    return "configuration_pending";
  if (s.monitor.phase != PRINTER_MONITOR_OFF)
    return "monitoring";
  if (!link->connected)
    return "disconnected";
  if (!link->state_known || link->state == PRINTER_RUN_UNKNOWN)
    return "state_unknown";
  if (!heat->enabled || heat->state == HEAT_STATE_FAULT ||
      heat->fault != HEAT_FAULT_NONE)
    return "heating_off";
  return "ready";
}
/* Caller holds the lock. */
static void publish(const app_status_t *heat, uint64_t now) {
  s.status.monitor = s.monitor;
  s.status.wait_remaining_ms = printer_monitor_wait_remaining(&s.monitor, now);
  snprintf(s.status.reason, sizeof(s.status.reason), "%s", reason_for(heat));
  s.status.can_monitor = !strcmp(s.status.reason, "ready");
}
static uint64_t permit_for(uint64_t now) {
  return printer_monitor_heat_allowed(&s.monitor) ? now + PRINTER_MONITOR_PERMIT_MS
                                                  : 0;
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
  if (!ops || !ops->heat_snapshot || !ops->monitor_begin ||
      !ops->monitor_permit || !ops->monitor_end || !ops->monitor_active ||
      !ops->link_configure || !ops->link_tick || !ops->link_status ||
      !ops->load_config || !ops->save_config || !ops->clear_config ||
      !ops->gate_begin || !ops->gate_end || !ops->now_ms)
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
  printer_monitor_init(&s.monitor);
  s.command = 0;
  s.configured = ops->load_config(&s.config, ops->ctx) == ESP_OK;
  if (s.configured)
    (void)ops->link_configure(&s.config, ops->ctx);
  else
    memset(&s.config, 0, sizeof(s.config));
  redacted_config();
  app_status_t heat;
  ops->heat_snapshot(&heat, ops->ctx);
  publish(&heat, ops->now_ms(ops->ctx));
  atomic_store(&started, true);
#ifndef PRINTER_CONTROL_MANUAL_POLL
  if (xTaskCreate(worker, "printer_control", 6144, NULL, 3, &s.task) !=
      pdPASS) {
    atomic_store(&started, false);
    return ESP_ERR_NO_MEM;
  }
#endif
  return ESP_OK;
}
esp_err_t printer_control_monitor_start(void) {
  if (!atomic_load(&started))
    return ESP_ERR_INVALID_STATE;
  if (!lock())
    return ESP_ERR_TIMEOUT;
  app_status_t heat;
  s.ops.heat_snapshot(&heat, s.ops.ctx);
  const uint64_t now = s.ops.now_ms(s.ops.ctx);
  publish(&heat, now);
  esp_err_t result = ESP_ERR_NOT_ALLOWED;
  if (s.status.can_monitor &&
      printer_monitor_start(&s.monitor, &s.status.link, now)) {
    result = s.ops.monitor_begin(permit_for(now), s.ops.ctx);
    if (result != ESP_OK)
      printer_monitor_init(&s.monitor);
  }
  publish(&heat, now);
  unlock();
  return result;
}
esp_err_t printer_control_monitor_stop(void) {
  if (!atomic_load(&started))
    return ESP_ERR_INVALID_STATE;
  if (!lock())
    return ESP_ERR_TIMEOUT;
  if (s.monitor.phase != PRINTER_MONITOR_OFF) {
    printer_monitor_stop(&s.monitor, PRINTER_MONITOR_END_STOPPED);
    s.ops.monitor_end(false, s.ops.ctx);
  }
  app_status_t heat;
  s.ops.heat_snapshot(&heat, s.ops.ctx);
  publish(&heat, s.ops.now_ms(s.ops.ctx));
  unlock();
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
    s.status.can_monitor = false;
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
        /* The controller can no longer tell when the watched print ends. */
        if (s.monitor.phase != PRINTER_MONITOR_OFF) {
          printer_monitor_stop(&s.monitor, PRINTER_MONITOR_END_SETTINGS);
          s.ops.monitor_end(true, s.ops.ctx);
        }
      }
      s.status.applying = false;
      strcpy(s.status.config_result,
             result == ESP_OK ? "saved" : "save_failed");
      s.completion_result = result;
      unlock();
    }
    s.ops.gate_end(SERVICE_OPERATION_PRINTER_CONFIG, s.ops.ctx);
    if (command == 2)
      xSemaphoreGive(s.completion);
  }
  s.ops.link_tick(s.ops.now_ms(s.ops.ctx), s.ops.ctx);
  printer_link_status_t link;
  s.ops.link_status(&link, s.ops.ctx);
  if (!lock())
    return;
  const uint64_t now = s.ops.now_ms(s.ops.ctx);
  app_status_t heat;
  s.ops.heat_snapshot(&heat, s.ops.ctx);
  s.status.link = link;
  if (s.monitor.phase != PRINTER_MONITOR_OFF) {
    if (!s.ops.monitor_active(s.ops.ctx)) {
      /* Disable, a fault or maintenance turned heating off in the runtime. */
      printer_monitor_stop(&s.monitor, PRINTER_MONITOR_END_HEATING_OFF);
    } else {
      printer_monitor_step(&s.monitor, &link, now);
      if (s.monitor.phase == PRINTER_MONITOR_OFF)
        s.ops.monitor_end(printer_monitor_end_disables_heating(s.monitor.last_end),
                          s.ops.ctx);
      else if (s.ops.monitor_permit(permit_for(now), s.ops.ctx) ==
               ESP_ERR_INVALID_STATE)
        printer_monitor_stop(&s.monitor, PRINTER_MONITOR_END_HEATING_OFF);
    }
  }
  publish(&heat, now);
  unlock();
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
  unlock();
}
