#pragma once
#include "firmware_manifest.h"
#include "firmware_transfer.h"
#include "esp_partition.h"
typedef struct { const esp_partition_t *partition; bool verified; } firmware_staged_t;
esp_err_t firmware_transport_check(const char *installed, firmware_candidate_t *out,
    firmware_check_result_t *out_result);
esp_err_t firmware_transport_stage(const firmware_candidate_t *candidate,
    firmware_progress_cb_t progress, void *ctx, firmware_staged_t *out);
esp_err_t firmware_transport_activate(const firmware_staged_t *staged);
