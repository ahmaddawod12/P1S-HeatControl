#include "firmware_transfer.h"
#include "firmware_policy.h"
#include <string.h>

esp_err_t firmware_transfer_stage(const firmware_manifest_t *manifest,
    const firmware_stream_ops_t *ops, firmware_progress_cb_t progress, void *ctx)
{
    if (!manifest || !ops || !ops->now_ms || !ops->read || !ops->begin || !ops->write ||
        !ops->finish || !ops->abort || !ops->hash_begin || !ops->hash_update || !ops->hash_finish ||
        !firmware_identity_valid(&manifest->identity) || manifest->size > FIRMWARE_SLOT_BYTES ||
        manifest->size < FIRMWARE_IDENTITY_OFFSET + sizeof(firmware_identity_t)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ops->begin(manifest->size, ops->ctx);
    if (err != ESP_OK) return err;
    bool live = true;
    const uint64_t started = ops->now_ms(ops->ctx);
    uint8_t buffer[4096], prefix[FIRMWARE_IDENTITY_OFFSET + sizeof(firmware_identity_t)];
    uint32_t received = 0;
    err = ops->hash_begin(ops->ctx);
    while (err == ESP_OK) {
        if (ops->now_ms(ops->ctx) - started >= 180000) { err = ESP_ERR_TIMEOUT; break; }
        const int count = ops->read(buffer, sizeof(buffer), ops->ctx);
        if (ops->now_ms(ops->ctx) - started >= 180000) { err = ESP_ERR_TIMEOUT; break; }
        if (count < 0) { err = ESP_FAIL; break; }
        if (count == 0) {
            if (received != manifest->size) err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if ((size_t) count > sizeof(buffer) || (uint32_t) count > manifest->size - received) {
            err = ESP_ERR_INVALID_SIZE; break;
        }
        if (received < sizeof(prefix)) {
            size_t keep = sizeof(prefix) - received;
            if (keep > (size_t) count) keep = (size_t) count;
            memcpy(prefix + received, buffer, keep);
        }
        err = ops->hash_update(buffer, (size_t) count, ops->ctx);
        if (err == ESP_OK) err = ops->write(buffer, (size_t) count, ops->ctx);
        if (err != ESP_OK) break;
        received += (uint32_t) count;
        if (progress) progress(received, manifest->size, ctx);
    }
    uint8_t digest[32];
    if (err == ESP_OK) err = ops->hash_finish(digest, ops->ctx);
    if (err == ESP_OK && memcmp(digest, manifest->sha256, 32) != 0) err = ESP_ERR_INVALID_RESPONSE;
    firmware_identity_t identity;
    if (err == ESP_OK) {
        memcpy(&identity, prefix + FIRMWARE_IDENTITY_OFFSET, sizeof(identity));
        if (!firmware_identity_matches(&identity, &manifest->identity)) err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        /* Native esp_ota_end consumes its handle even on signature failure. */
        live = false;
        err = ops->finish(ops->ctx);
    }
    if (live) ops->abort(ops->ctx);
    return err;
}
