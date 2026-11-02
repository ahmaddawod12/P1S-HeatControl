#include "printer_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "printer_status.h"
#include "printer_transport.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
typedef struct {
  uint32_t epoch, report_revision;
  unsigned type;
  uint64_t at;
  printer_report_delta_t delta;
} link_event_t;
static struct {
  printer_link_ops_t ops;
  printer_connection_config_t config;
  printer_status_t status;
  QueueHandle_t events;
  uint32_t epoch;
  uint64_t retry_at, last_snapshot;
  unsigned backoff;
  bool configured, attempting, connected, subscribed, have_snapshot;
  uint64_t last_resume_id;
} s;
static atomic_uint generation;
static atomic_bool suspended, resources;
uint32_t printer_link_generation(void) { return atomic_load(&generation); }
bool printer_link_connection_live(uint32_t epoch) {
  return !printer_link_cancelled(epoch) && atomic_load(&resources);
}
bool printer_link_cancelled(uint32_t epoch) {
  return atomic_load(&suspended) || epoch != printer_link_generation();
}
void printer_link_invalidate(uint32_t epoch) {
  unsigned expected = epoch;
  (void)atomic_compare_exchange_strong(&generation, &expected, epoch + 1);
}
static void enqueue(link_event_t *event) {
  if (printer_link_cancelled(event->epoch))
    return;
  if (xQueueSend(s.events, event, 0) != pdTRUE)
    printer_link_invalidate(event->epoch);
}
void printer_link_event_connected(uint32_t epoch) {
  link_event_t e = {.epoch = epoch, .type = 1};
  enqueue(&e);
}
void printer_link_event_subscribed(uint32_t epoch) {
  link_event_t e = {.epoch = epoch, .type = 2};
  enqueue(&e);
}
void printer_link_event_report(uint32_t epoch, const char *json, size_t len,
                               uint32_t revision, uint64_t now) {
  if (printer_link_cancelled(epoch))
    return;
  link_event_t e = {
      .epoch = epoch, .type = 3, .at = now, .report_revision = revision};
  esp_err_t err = printer_report_parse(json, len, &e.delta);
  if (err == ESP_OK)
    enqueue(&e);
  else if (err != ESP_ERR_NOT_FOUND)
    printer_link_invalidate(epoch);
}
esp_err_t printer_link_start_with_ops(const printer_link_ops_t *ops) {
  if (!ops || !ops->connect || !ops->subscribe || !ops->publish_live ||
      !ops->abort || !ops->now_ms)
    return ESP_ERR_INVALID_ARG;
  if (!s.events)
    s.events = xQueueCreate(4, sizeof(link_event_t));
  if (!s.events)
    return ESP_ERR_NO_MEM;
  s.ops = *ops;
  s.epoch = atomic_fetch_add(&generation, 1) + 1;
  s.configured = s.attempting = s.connected = s.subscribed = s.have_snapshot =
      false;
  s.backoff = 0;
  s.retry_at = 0;
  atomic_store(&suspended, false);
  atomic_store(&resources, false);
  xQueueReset(s.events);
  printer_status_init(&s.status, s.epoch);
  return ESP_OK;
}
esp_err_t printer_link_start(void) {
  return printer_link_start_with_ops(printer_transport_ops());
}
esp_err_t printer_link_configure(const printer_connection_config_t *config) {
  if (!s.events || !config)
    return ESP_ERR_INVALID_ARG;
  bool different = strcmp(s.config.serial, config->serial) != 0;
  printer_link_invalidate(printer_link_generation());
  s.config = *config;
  s.configured = true;
  s.backoff = 0;
  s.retry_at = 0;
  if (different)
    s.have_snapshot = false;
  return ESP_OK;
}
void printer_link_suspend(bool value) {
  if (atomic_exchange(&suspended, value) != value)
    printer_link_invalidate(printer_link_generation());
}
bool printer_link_quiescent(void) { return !atomic_load(&resources); }
void printer_link_cancel_resume(void) {
  printer_link_invalidate(printer_link_generation());
}
printer_dispatch_result_t
printer_link_resume_once(uint64_t id, const printer_job_key_t *expected,
                         const printer_safety_history_t *history,
                         uint32_t epoch, uint64_t deadline) {
  uint64_t now = s.ops.now_ms(s.ops.ctx);
  if (!id || id <= s.last_resume_id || !expected || !history || deadline <= now ||
      deadline - now > 1000)
    return PRINTER_DISPATCH_REJECTED;
  printer_link_tick(now);
  printer_observation_t observation;
  printer_link_snapshot(&observation);
  if (!observation.startup_eligible || observation.connection_epoch != epoch ||
      !printer_job_same(expected, &observation.job) ||
      observation.history.pause != history->pause ||
      observation.history.error != history->error ||
      observation.history.identity != history->identity ||
      printer_link_cancelled(epoch) || s.ops.now_ms(s.ops.ctx) >= deadline)
    return PRINTER_DISPATCH_REJECTED;
  s.last_resume_id = id;
  char topic[64], json[128];
  snprintf(topic, sizeof(topic), "device/%s/request", s.config.serial);
  int n = snprintf(json, sizeof(json),
                   "{\"print\":{\"sequence_id\":\"%llu\",\"command\":"
                   "\"resume\",\"param\":\"\"}}",
                   (unsigned long long)id);
  printer_dispatch_result_t result =
      s.ops.publish_live(topic, json, (size_t)n, 0, false, deadline,
                         observation.report_revision, s.ops.ctx);
  if (result != PRINTER_DISPATCH_SENT)
    printer_link_invalidate(epoch);
  return result;
}
static void retry(uint64_t now) {
  static const unsigned delays[] = {1000, 2000, 4000, 8000, 30000};
  s.retry_at = now + delays[s.backoff];
  if (s.backoff < 4)
    ++s.backoff;
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
    printer_status_init(&s.status, s.epoch);
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
    if (e.type == 1) {
      char topic[64];
      snprintf(topic, sizeof(topic), "device/%s/report", s.config.serial);
      s.connected = true;
      s.backoff = 0;
      if (s.ops.subscribe(topic, s.ops.ctx) != ESP_OK)
        printer_link_invalidate(s.epoch);
    } else if (e.type == 2 && s.connected) {
      s.subscribed = true;
    } else if (e.type == 3 && s.connected && s.subscribed) {
      if (printer_status_apply(&s.status, &e.delta, e.epoch, e.at))
        s.status.report_revision = e.report_revision;
    }
  }
  /* Preserve the initial snapshot even when decision fields arrive first.
   * Later ticks recover missing baselines or active-job identity, within the
   * same attempt budget and without polling complete or idle baselines. */
  if (s.connected && s.subscribed && !printer_link_cancelled(s.epoch) &&
      (!s.have_snapshot ||
       (printer_status_snapshot_required(&s.status) && now >= s.last_snapshot &&
        now - s.last_snapshot >= 300000))) {
    char topic[64];
    snprintf(topic, sizeof(topic), "device/%s/request", s.config.serial);
    const char json[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":"
                        "\"pushall\",\"version\":1,\"push_target\":1}}";
    /* Budget attempts too: a failed write must not make reconnects flood
     * pushall. */
    s.have_snapshot = true;
    s.last_snapshot = now;
    (void)s.ops.publish_live(topic, json, sizeof(json) - 1, 0, false,
                             now + 1000, 0, s.ops.ctx);
  }
}
void printer_link_snapshot(printer_observation_t *out) {
  printer_status_snapshot(&s.status, s.ops.now_ms(s.ops.ctx), out);
  if (!s.connected || printer_link_cancelled(s.epoch)) {
    out->connected = out->fresh = out->baseline_valid = out->startup_eligible =
        false;
    strcpy(out->reason, atomic_load(&suspended) ? "suspended" : "disconnected");
  }
}
