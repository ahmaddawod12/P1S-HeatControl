#pragma once
#include "firmware_manifest.h"
#include "firmware_transfer.h"
typedef struct {
    void *ctx;
    esp_err_t (*check)(const char *, firmware_candidate_t *, firmware_check_result_t *, void *);
    esp_err_t (*begin_maintenance)(uint32_t, void *);
    esp_err_t (*end_maintenance)(void *);
    esp_err_t (*stage)(const firmware_candidate_t *, firmware_progress_cb_t, void *, void *);
    esp_err_t (*load_attempt)(firmware_attempt_t *, void *);
    esp_err_t (*save_attempt)(const firmware_attempt_t *, void *);
    esp_err_t (*activate)(void *);
    void (*restart)(void *);
} firmware_job_ops_t;
typedef void (*firmware_status_cb_t)(const firmware_status_t *, void *);
void firmware_job_run(firmware_status_t *status, const firmware_job_ops_t *ops,
    firmware_status_cb_t publish, void *ctx);
