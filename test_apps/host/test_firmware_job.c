#include "firmware_job.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char trace[32]; unsigned used, saves;
    bool current, check_fail, maintenance_fail, stage_fail, save_fail, activate_fail, cleanup_fail;
    firmware_attempt_t persisted;
} fake_t;
static void note(fake_t *f, char c) { f->trace[f->used++] = c; f->trace[f->used] = 0; }
static esp_err_t check(const char *installed, firmware_candidate_t *out, firmware_check_result_t *result, void *ctx)
{
    fake_t *f = ctx; assert(strcmp(installed, "1.0.0") == 0); note(f, 'C');
    *result = f->current ? FIRMWARE_CHECK_UP_TO_DATE : FIRMWARE_CHECK_AVAILABLE;
    out->manifest.identity = (firmware_identity_t) {FIRMWARE_IDENTITY_MAGIC, FIRMWARE_IDENTITY_FORMAT,
        FIRMWARE_PRODUCT, FIRMWARE_HARDWARE, FIRMWARE_LAYOUT, "1.0.1", 0};
    return f->check_fail ? ESP_FAIL : ESP_OK;
}
static esp_err_t maintenance(uint32_t timeout, void *ctx)
{ fake_t *f = ctx; assert(timeout == 15000); note(f, 'M'); return f->maintenance_fail ? ESP_FAIL : ESP_OK; }
static esp_err_t cleanup(void *ctx)
{ fake_t *f = ctx; note(f, 'E'); return f->cleanup_fail ? ESP_FAIL : ESP_OK; }
static esp_err_t stage(const firmware_candidate_t *candidate, firmware_progress_cb_t progress, void *pctx, void *ctx)
{ fake_t *f = ctx; (void) candidate; note(f, 'D'); progress(512, 512, pctx); return f->stage_fail ? ESP_FAIL : ESP_OK; }
static esp_err_t load(firmware_attempt_t *out, void *ctx)
{ (void) out; (void) ctx; return ESP_ERR_NOT_FOUND; }
static esp_err_t save(const firmware_attempt_t *attempt, void *ctx)
{ fake_t *f = ctx; note(f, 'S'); f->saves++; if (f->save_fail) return ESP_FAIL; f->persisted = *attempt; return ESP_OK; }
static esp_err_t activate(void *ctx)
{ fake_t *f = ctx; note(f, 'A'); return f->activate_fail ? ESP_FAIL : ESP_OK; }
static void restart(void *ctx) { note(ctx, 'R'); }
static firmware_status_t run(fake_t *f)
{
    firmware_status_t status = {.installed = "1.0.0"};
    const firmware_job_ops_t ops = {.ctx = f, .check = check, .begin_maintenance = maintenance,
        .end_maintenance = cleanup, .stage = stage, .load_attempt = load,
        .save_attempt = save, .activate = activate, .restart = restart};
    firmware_job_run(&status, &ops, NULL, NULL);
    return status;
}
int main(void)
{
    fake_t f = {.current = true}; firmware_status_t status = run(&f);
    assert(strcmp(f.trace, "C") == 0 && status.result == FIRMWARE_RESULT_UP_TO_DATE);
    f = (fake_t) {.check_fail = true}; status = run(&f);
    assert(strcmp(f.trace, "C") == 0 && status.phase == FIRMWARE_FAILED);
    f = (fake_t) {0}; status = run(&f);
    assert(strcmp(f.trace, "CSMDSAR") == 0 && status.phase == FIRMWARE_REBOOTING);
    assert(f.persisted.id == 1 && strcmp(f.persisted.previous, "1.0.0") == 0);
    assert(f.persisted.result != FIRMWARE_RESULT_INSTALLED);
    f = (fake_t) {.stage_fail = true}; status = run(&f);
    assert(strchr(f.trace, 'A') == NULL && strchr(f.trace, 'R') == NULL && strchr(f.trace, 'E'));
    assert(status.phase == FIRMWARE_FAILED && !status.maintenance);
    f = (fake_t) {.save_fail = true}; status = run(&f);
    assert(strcmp(f.trace, "CS") == 0 && status.result == FIRMWARE_RESULT_STORAGE_FAILED);
    f = (fake_t) {.activate_fail = true}; status = run(&f);
    assert(strchr(f.trace, 'R') == NULL && status.phase == FIRMWARE_FAILED);
    f = (fake_t) {.maintenance_fail = true, .cleanup_fail = true}; status = run(&f);
    assert(status.maintenance && status.result == FIRMWARE_RESULT_UNSAFE);
    puts("firmware job passed");
    return 0;
}
