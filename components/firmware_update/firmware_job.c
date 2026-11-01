#include "firmware_job.h"
#include "firmware_policy.h"
#include <stdlib.h>
#include <string.h>

typedef struct { firmware_status_t *status; firmware_status_cb_t publish; void *ctx; } progress_t;
static void changed(progress_t *p) { if (p->publish) p->publish(p->status, p->ctx); }
static void progress(uint32_t received, uint32_t total, void *ctx)
{
    progress_t *p = ctx; p->status->received = received; p->status->total = total;
    p->status->phase = received == total ? FIRMWARE_VERIFYING : FIRMWARE_DOWNLOADING;
    changed(p);
}
void firmware_job_run(firmware_status_t *status, const firmware_job_ops_t *ops,
    firmware_status_cb_t publish, void *ctx)
{
    if (!status || !ops || !ops->check || !ops->begin_maintenance || !ops->end_maintenance ||
        !ops->stage || !ops->load_attempt || !ops->save_attempt || !ops->activate || !ops->restart) return;
    progress_t update = {status, publish, ctx};
    status->phase = FIRMWARE_CHECKING; status->result = FIRMWARE_RESULT_NONE;
    status->received = status->total = 0; status->candidate[0] = 0; changed(&update);
    firmware_candidate_t *candidate = calloc(1, sizeof(*candidate));
    firmware_result_t failure = FIRMWARE_RESULT_CHECK_FAILED;
    bool maintenance = false, recorded = false;
    firmware_attempt_t attempt = {0};
    if (!candidate) goto failed;
    firmware_check_result_t check_result;
    if (ops->check(status->installed, candidate, &check_result, ops->ctx) != ESP_OK) goto failed;
    if (check_result != FIRMWARE_CHECK_AVAILABLE) {
        status->phase = FIRMWARE_IDLE;
        status->result = check_result == FIRMWARE_CHECK_UP_TO_DATE ? FIRMWARE_RESULT_UP_TO_DATE : FIRMWARE_RESULT_NO_RELEASE;
        free(candidate); changed(&update); return;
    }
    firmware_version_t installed, proposed;
    if (!firmware_identity_valid(&candidate->manifest.identity) ||
        !firmware_version_parse(status->installed, &installed) ||
        !firmware_version_parse(candidate->manifest.identity.version, &proposed) ||
        firmware_version_compare(&proposed, &installed) <= 0) goto failed;
    strcpy(status->candidate, candidate->manifest.identity.version);
    esp_err_t loaded = ops->load_attempt(&attempt, ops->ctx);
    failure = FIRMWARE_RESULT_STORAGE_FAILED;
    if ((loaded != ESP_OK && loaded != ESP_ERR_NOT_FOUND) || attempt.id == UINT32_MAX) goto failed;
    const uint32_t id = loaded == ESP_OK ? attempt.id + 1 : 1;
    attempt = (firmware_attempt_t) {.format = 1, .id = id, .phase = FIRMWARE_COOLING};
    strcpy(attempt.previous, status->installed); strcpy(attempt.candidate, status->candidate);
    if (ops->save_attempt(&attempt, ops->ctx) != ESP_OK) goto failed;
    recorded = true;
    status->phase = FIRMWARE_COOLING; status->maintenance = true; changed(&update);
    maintenance = true; failure = FIRMWARE_RESULT_UNSAFE;
    if (ops->begin_maintenance(15000, ops->ctx) != ESP_OK) goto failed;
    status->phase = FIRMWARE_DOWNLOADING; changed(&update);
    failure = FIRMWARE_RESULT_DOWNLOAD_FAILED;
    if (ops->stage(candidate, progress, &update, ops->ctx) != ESP_OK) goto failed;
    status->phase = FIRMWARE_VERIFYING; changed(&update);
    attempt.phase = FIRMWARE_REBOOTING;
    failure = FIRMWARE_RESULT_STORAGE_FAILED;
    if (ops->save_attempt(&attempt, ops->ctx) != ESP_OK) goto failed;
    failure = FIRMWARE_RESULT_INVALID_IMAGE;
    if (ops->activate(ops->ctx) != ESP_OK) goto failed;
    status->phase = FIRMWARE_REBOOTING; changed(&update);
    free(candidate); ops->restart(ops->ctx); return;
failed:
    free(candidate);
    if (maintenance) {
        status->maintenance = ops->end_maintenance(ops->ctx) != ESP_OK;
        if (status->maintenance) failure = FIRMWARE_RESULT_UNSAFE;
    }
    status->phase = FIRMWARE_FAILED; status->result = failure;
    if (recorded) { attempt.phase = FIRMWARE_FAILED; attempt.result = failure; (void) ops->save_attempt(&attempt, ops->ctx); }
    changed(&update);
}
