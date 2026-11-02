#include "printer_status.h"
#include <string.h>
static bool terminal(printer_run_state_t s) {
  return s == PRINTER_RUN_IDLE || s == PRINTER_RUN_FINISHED ||
         s == PRINTER_RUN_FAILED;
}
static bool active(printer_run_state_t s) {
  return s == PRINTER_RUN_RUNNING || s == PRINTER_RUN_PREPARING ||
         s == PRINTER_RUN_PAUSED;
}
static bool identity_valid(const printer_status_t *s) {
  return (s->values.file[0] || s->values.label[0]) &&
         (s->values.start_time || s->observed_run);
}
void printer_status_init(printer_status_t *s, uint32_t epoch) {
  memset(s, 0, sizeof(*s));
  s->epoch = epoch;
}
bool printer_status_apply(printer_status_t *s, const printer_report_delta_t *d,
                          uint32_t epoch, uint64_t now) {
  if (!s || !d || !d->present || epoch != s->epoch)
    return false;
  bool new_run = (d->present & PRINTER_FIELD_STATE) && active(d->state) &&
                 (!active(s->values.state) || s->observed_terminal);
  if ((d->present & PRINTER_FIELD_START) && d->start_time &&
      s->values.start_time && d->start_time != s->values.start_time)
    new_run = true;
  if (new_run) {
    s->print_epoch++;
    s->observed_run = s->observed_terminal;
    s->observed_terminal = false;
    s->history = (printer_safety_history_t){0};
    s->observed_pause = s->left_initial_pause = false;
    memset(&s->values, 0, sizeof(s->values));
  }
  if ((d->present & PRINTER_FIELD_STATE) && d->state != s->values.state) {
    if (d->state == PRINTER_RUN_PAUSED) {
      if (s->observed_pause)
        ++s->history.pause;
      s->observed_pause = true;
    } else if (s->values.state == PRINTER_RUN_PAUSED &&
               (d->state == PRINTER_RUN_RUNNING ||
                d->state == PRINTER_RUN_PREPARING)) {
      s->left_initial_pause = true;
    }
  }
  if (((d->present & PRINTER_FIELD_ERROR) && d->error) ||
      ((d->present & PRINTER_FIELD_HMS) && d->hms_blocking))
    ++s->history.error;
#define MERGE(bit, field)                                                      \
  if (d->present & (bit))                                                      \
  s->values.field = d->field
  MERGE(PRINTER_FIELD_STATE, state);
  MERGE(PRINTER_FIELD_LAYER, layer);
  MERGE(PRINTER_FIELD_START, start_time);
  MERGE(PRINTER_FIELD_PROGRESS, progress);
  MERGE(PRINTER_FIELD_REMAINING, remaining);
  MERGE(PRINTER_FIELD_BED, bed_c);
  MERGE(PRINTER_FIELD_NOZZLE, nozzle_c);
  MERGE(PRINTER_FIELD_ERROR, error);
  MERGE(PRINTER_FIELD_HMS, hms_blocking);
#undef MERGE
#define COPY(bit, field)                                                       \
  if (d->present & (bit)) {                                                    \
    if (s->values.field[0] && strcmp(s->values.field, d->field))               \
      ++s->history.identity;                                                   \
    memcpy(s->values.field, d->field, sizeof(d->field));                       \
  }
  COPY(PRINTER_FIELD_TASK, task);
  COPY(PRINTER_FIELD_SUBTASK, subtask);
  COPY(PRINTER_FIELD_FILE, file);
  COPY(PRINTER_FIELD_LABEL, label);
#undef COPY
  if (d->present & PRINTER_FIELD_TYPE)
    memcpy(s->values.print_type, d->print_type, sizeof(d->print_type));
  s->values.present |= d->present;
  s->received_ms = now;
  ++s->report_revision;
  s->have_report = true;
  if ((d->present & PRINTER_FIELD_STATE) && terminal(d->state))
    s->observed_terminal = true;
  return true;
}
bool printer_status_baseline_valid(const printer_status_t *s) {
  const uint32_t required = PRINTER_FIELD_STATE | PRINTER_FIELD_ERROR |
                            PRINTER_FIELD_HMS | PRINTER_FIELD_LAYER;
  return (s->values.present & required) == required;
}
bool printer_status_snapshot_required(const printer_status_t *s) {
  return !printer_status_baseline_valid(s) ||
         (active(s->values.state) && !identity_valid(s));
}
void printer_status_snapshot(const printer_status_t *s, uint64_t now,
                             printer_observation_t *o) {
  memset(o, 0, sizeof(*o));
  o->state = s->values.state;
  o->history = s->history;
  o->connection_epoch = s->epoch;
  o->received_ms = s->received_ms;
  o->valid_mask = s->values.present;
  o->report_revision = s->report_revision;
  o->connected = true;
  o->fresh = s->have_report && now >= s->received_ms &&
             now - s->received_ms < PRINTER_TELEMETRY_MS;
  o->baseline_valid = printer_status_baseline_valid(s);
  o->blocking_error = s->values.error || s->values.hms_blocking;
  o->job.connection_epoch = s->epoch;
  o->job.print_epoch = s->print_epoch;
  o->job.start_time = s->values.start_time;
  memcpy(o->job.file, s->values.file, sizeof(o->job.file));
  memcpy(o->job.label, s->values.label, sizeof(o->job.label));
  memcpy(o->job.task, s->values.task, sizeof(o->job.task));
  memcpy(o->job.subtask, s->values.subtask, sizeof(o->job.subtask));
  o->job.identity_valid = identity_valid(s);
  o->startup_eligible = o->fresh && o->baseline_valid && !o->blocking_error &&
                        o->job.identity_valid &&
                        o->state == PRINTER_RUN_PAUSED &&
                        s->values.layer == 0 && !s->left_initial_pause;
  o->progress = s->values.progress;
  o->remaining = s->values.remaining;
  o->progress_valid = (s->values.present & PRINTER_FIELD_PROGRESS) != 0;
  o->remaining_valid = (s->values.present & PRINTER_FIELD_REMAINING) != 0;
  o->bed_c = s->values.bed_c;
  o->nozzle_c = s->values.nozzle_c;
  strcpy(o->reason, !o->fresh                ? "telemetry_stale"
                    : !o->baseline_valid     ? "baseline_required"
                    : o->blocking_error      ? "printer_error"
                    : !o->job.identity_valid ? "identity_required"
                    : !o->startup_eligible   ? "initial_pause_required"
                                             : "ready");
}
static bool compatible(const char *a, const char *b) {
  return !a[0] || !b[0] || !strcmp(a, b);
}
bool printer_job_same(const printer_job_key_t *a, const printer_job_key_t *b) {
  return a && b && a->identity_valid && b->identity_valid &&
         a->connection_epoch == b->connection_epoch &&
         a->print_epoch == b->print_epoch && a->start_time == b->start_time &&
         compatible(a->file, b->file) && compatible(a->label, b->label) &&
         compatible(a->task, b->task) && compatible(a->subtask, b->subtask);
}
