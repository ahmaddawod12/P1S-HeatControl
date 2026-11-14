#include "printer_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "printer_report.h"
#include "printer_transport.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
enum { EVENT_CONNECTED = 1, EVENT_SUBSCRIBED = 2 };
typedef struct {
  uint32_t epoch;
  unsigned type;
} link_event_t;
/* Reports from the MQTT task merge here without a queue. Print history
 * survives newer states, unreadable reports and reconnects; memory is bounded. */
typedef struct {
  uint32_t epoch;
  bool have_report, have_state, invalid;
  printer_run_state_t state;
  uint64_t at;
  bool print_active;
  uint64_t started_prints, completed_prints;
  printer_run_state_t last_completed_state;
} mailbox_t;
static struct {
  printer_link_ops_t ops;
  printer_connection_config_t config;
  printer_link_status_t status;
  QueueHandle_t events;
  SemaphoreHandle_t mailbox_lock;
  mailbox_t mailbox;
  uint32_t epoch, connections;
  uint64_t retry_at, last_pushall;
  unsigned backoff;
  bool configured, attempting, connected, subscribed, pushall_sent;
} s;
static atomic_uint generation;
static atomic_bool suspended, resources;
uint32_t printer_link_generation(void) { return atomic_load(&generation); }
bool printer_link_cancelled(uint32_t epoch) {
  return atomic_load(&suspended) || epoch != printer_link_generation();
}
void printer_link_invalidate(uint32_t epoch) {
  unsigned expected = epoch;
  (void)atomic_compare_exchange_strong(&generation, &expected, epoch + 1);
}
static void enqueue(uint32_t epoch, unsigned type) {
  if (printer_link_cancelled(epoch))
    return;
  link_event_t e = {.epoch = epoch, .type = type};
  if (xQueueSend(s.events, &e, 0) != pdTRUE)
    printer_link_invalidate(epoch);
}
void printer_link_event_connected(uint32_t epoch) {
  enqueue(epoch, EVENT_CONNECTED);
}
void printer_link_event_subscribed(uint32_t epoch) {
  enqueue(epoch, EVENT_SUBSCRIBED);
}
static void mailbox_put(uint32_t epoch, const printer_report_t *report,
                        uint64_t now) {
  if (printer_link_cancelled(epoch) ||
      xSemaphoreTake(s.mailbox_lock, portMAX_DELAY) != pdTRUE)
    return;
  mailbox_t *m = &s.mailbox;
  if (printer_link_cancelled(epoch)) {
    xSemaphoreGive(s.mailbox_lock);
    return;
  }
  if (m->epoch != epoch) {
    m->epoch = epoch;
    m->have_report = m->have_state = m->invalid = false;
  }
  if (!report) {
    m->invalid = true;
    m->have_state = false;
  } else {
    m->have_report = true;
    m->at = now;
    if (report->has_state) {
      m->have_state = true;
      m->invalid = false;
      m->state = report->state;
      if (report->state == PRINTER_RUN_PREPARING ||
          report->state == PRINTER_RUN_RUNNING ||
          report->state == PRINTER_RUN_PAUSED) {
        if (!m->print_active)
          ++m->started_prints;
        m->print_active = true;
      } else if (m->print_active &&
                 (report->state == PRINTER_RUN_FINISHED ||
                  report->state == PRINTER_RUN_FAILED ||
                  report->state == PRINTER_RUN_IDLE)) {
        ++m->completed_prints;
        m->last_completed_state = report->state;
        m->print_active = false;
      }
    }
  }
  xSemaphoreGive(s.mailbox_lock);
}
void printer_link_event_report(uint32_t epoch, const char *json, size_t len,
                               uint64_t now) {
  printer_report_t report;
  const esp_err_t err = printer_report_parse(json, len, &report);
  if (err == ESP_OK)
    mailbox_put(epoch, &report, now);
  else if (err != ESP_ERR_NOT_FOUND)
    mailbox_put(epoch, NULL, now);
}
void printer_link_event_report_rejected(uint32_t epoch) {
  mailbox_put(epoch, NULL, 0);
}
static void reset_status(void) {
  const uint32_t reconnects = s.connections > 1 ? s.connections - 1 : 0;
  xSemaphoreTake(s.mailbox_lock, portMAX_DELAY);
  s.status = (printer_link_status_t){
      .reconnects = reconnects,
      .connection_epoch = s.epoch,
      .started_prints = s.status.started_prints,
      .completed_prints = s.status.completed_prints,
      .last_completed_state = s.status.last_completed_state};
  xSemaphoreGive(s.mailbox_lock);
}
esp_err_t printer_link_start_with_ops(const printer_link_ops_t *ops) {
  if (!ops || !ops->connect || !ops->subscribe || !ops->publish ||
      !ops->abort || !ops->now_ms)
    return ESP_ERR_INVALID_ARG;
  if (!s.events)
    s.events = xQueueCreate(4, sizeof(link_event_t));
  if (!s.mailbox_lock)
    s.mailbox_lock = xSemaphoreCreateMutex();
  if (!s.events || !s.mailbox_lock)
    return ESP_ERR_NO_MEM;
  s.ops = *ops;
  s.epoch = atomic_fetch_add(&generation, 1) + 1;
  s.configured = s.attempting = s.connected = s.subscribed = false;
  s.pushall_sent = false;
  s.connections = 0;
  s.backoff = 0;
  s.retry_at = s.last_pushall = 0;
  s.mailbox = (mailbox_t){0};
  memset(&s.status, 0, sizeof(s.status));
  atomic_store(&suspended, false);
  atomic_store(&resources, false);
  xQueueReset(s.events);
  reset_status();
  return ESP_OK;
}
esp_err_t printer_link_start(void) {
  return printer_link_start_with_ops(printer_transport_ops());
}
esp_err_t printer_link_configure(const printer_connection_config_t *config) {
  if (!s.events || !config)
    return ESP_ERR_INVALID_ARG;
  const bool different = strcmp(s.config.serial, config->serial) != 0;
  printer_link_invalidate(printer_link_generation());
  s.config = *config;
  s.configured = true;
  s.backoff = 0;
  s.retry_at = 0;
  if (different) {
    s.pushall_sent = false;
    xSemaphoreTake(s.mailbox_lock, portMAX_DELAY);
    s.mailbox.print_active = false;
    xSemaphoreGive(s.mailbox_lock);
  }
  return ESP_OK;
}
void printer_link_suspend(bool value) {
  if (atomic_exchange(&suspended, value) != value)
    printer_link_invalidate(printer_link_generation());
}
bool printer_link_quiescent(void) { return !atomic_load(&resources); }
static void retry(uint64_t now) {
  static const unsigned delays[] = {1000, 2000, 4000, 8000, 30000};
  s.retry_at = now + delays[s.backoff];
  if (s.backoff < 4)
    ++s.backoff;
}
/* Caller holds mailbox_lock. Used by the worker and the live start snapshot. */
static void merge_mailbox(printer_link_status_t *status, const mailbox_t *m) {
  status->started_prints = m->started_prints;
  status->completed_prints = m->completed_prints;
  status->last_completed_state = m->last_completed_state;
  if (m->epoch != status->connection_epoch)
    return;
  if (m->invalid) {
    status->state_known = false;
    status->state = PRINTER_RUN_UNKNOWN;
  } else if (m->have_state) {
    status->state_known = m->state != PRINTER_RUN_UNKNOWN;
    status->state = m->state;
  }
  if (m->have_report)
    status->received_ms = m->at;
}
static void take_mailbox(void) {
  xSemaphoreTake(s.mailbox_lock, portMAX_DELAY);
  merge_mailbox(&s.status, &s.mailbox);
  s.mailbox.have_report = s.mailbox.have_state = s.mailbox.invalid = false;
  xSemaphoreGive(s.mailbox_lock);
}
void printer_link_tick(uint64_t now) {
  if (!s.events)
    return;
  if (s.epoch != printer_link_generation()) {
    bool was_attempting = s.attempting;
    s.ops.abort(s.ops.ctx);
    atomic_store(&resources, false);
    s.epoch = printer_link_generation();
    s.attempting = s.connected = s.subscribed = false;
    reset_status();
    xQueueReset(s.events);
    if (was_attempting)
      retry(now);
  }
  if (atomic_load(&suspended) || !s.configured || !s.config.enabled)
    return;
  if (!s.attempting && now >= s.retry_at) {
    s.attempting = true;
    atomic_store(&resources, true);
    if (s.ops.connect(&s.config, s.ops.ctx) != ESP_OK) {
      printer_link_invalidate(s.epoch);
      return;
    }
  }
  link_event_t e;
  while (xQueueReceive(s.events, &e, 0) == pdTRUE) {
    if (e.epoch != s.epoch || printer_link_cancelled(s.epoch))
      continue;
    if (e.type == EVENT_CONNECTED && !s.connected) {
      char topic[64];
      snprintf(topic, sizeof(topic), "device/%s/report", s.config.serial);
      s.connected = true;
      s.backoff = 0;
      ++s.connections;
      reset_status();
      if (s.ops.subscribe(topic, s.ops.ctx) != ESP_OK)
        printer_link_invalidate(s.epoch);
    } else if (e.type == EVENT_SUBSCRIBED && s.connected) {
      s.subscribed = true;
    }
  }
  take_mailbox();
  xSemaphoreTake(s.mailbox_lock, portMAX_DELAY);
  s.status.connected =
      s.connected && s.subscribed && !printer_link_cancelled(s.epoch);
  const bool needs_recovery = s.status.connected && !s.status.state_known;
  xSemaphoreGive(s.mailbox_lock);
  if (needs_recovery &&
      (!s.pushall_sent || now - s.last_pushall >= PRINTER_PUSHALL_MIN_INTERVAL_MS)) {
    char topic[64];
    snprintf(topic, sizeof(topic), "device/%s/request", s.config.serial);
    const char json[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":"
                        "\"pushall\",\"version\":1,\"push_target\":1}}";
    /* Count the attempt itself: a failed write must not make retries flood. */
    s.pushall_sent = true;
    s.last_pushall = now;
    (void)s.ops.publish(topic, json, sizeof(json) - 1, now + 1000, s.ops.ctx);
  }
}
void printer_link_get_status(printer_link_status_t *out) {
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  if (!s.mailbox_lock)
    return;
  xSemaphoreTake(s.mailbox_lock, portMAX_DELAY);
  *out = s.status;
  /* Include reports received before this snapshot, even before the next tick.
   * This is the boundary between historical and newly monitored transitions. */
  merge_mailbox(out, &s.mailbox);
  xSemaphoreGive(s.mailbox_lock);
  if (printer_link_cancelled(out->connection_epoch))
    out->connected = false;
}
