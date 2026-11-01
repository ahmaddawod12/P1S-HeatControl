#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "firmware_types.h"
#define FIRMWARE_IDENTITY_OFFSET 288U
typedef void (*firmware_progress_cb_t)(uint32_t received, uint32_t total, void *ctx);
typedef struct {
    void *ctx;
    uint64_t (*now_ms)(void *);
    int (*read)(uint8_t *, size_t, void *);
    esp_err_t (*begin)(uint32_t, void *);
    esp_err_t (*write)(const uint8_t *, size_t, void *);
    esp_err_t (*finish)(void *);
    void (*abort)(void *);
    esp_err_t (*hash_begin)(void *);
    esp_err_t (*hash_update)(const uint8_t *, size_t, void *);
    esp_err_t (*hash_finish)(uint8_t *, void *);
} firmware_stream_ops_t;
esp_err_t firmware_transfer_stage(const firmware_manifest_t *manifest,
    const firmware_stream_ops_t *ops, firmware_progress_cb_t progress, void *ctx);
