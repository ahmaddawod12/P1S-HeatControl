#include "firmware_transfer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t bytes[513]; uint32_t pos, length, written, hashed;
    unsigned begin_calls, finish_calls, abort_calls;
    bool bad_hash, read_failure, finish_failure, expired;
} fake_t;
static uint64_t tick(void *ctx)
{ fake_t *f = ctx; return f->expired && f->pos ? 180000 : 0; }
static int read_bytes(uint8_t *out, size_t capacity, void *ctx)
{
    fake_t *f = ctx;
    if (f->read_failure && f->pos >= 7) return -1;
    size_t n = f->length - f->pos;
    if (n > 7) n = 7;
    if (n > capacity) n = capacity;
    memcpy(out, f->bytes + f->pos, n); f->pos += (uint32_t) n;
    return (int) n;
}
static esp_err_t begin(uint32_t size, void *ctx)
{ assert(size == 512); ((fake_t *) ctx)->begin_calls++; return ESP_OK; }
static esp_err_t write_bytes(const uint8_t *p, size_t n, void *ctx)
{ fake_t *f = ctx; assert(memcmp(p, f->bytes + f->written, n) == 0); f->written += (uint32_t) n; return ESP_OK; }
static esp_err_t finish(void *ctx)
{ fake_t *f = ctx; f->finish_calls++; return f->finish_failure ? ESP_FAIL : ESP_OK; }
static void abort_transfer(void *ctx) { ((fake_t *) ctx)->abort_calls++; }
static esp_err_t hash_begin(void *ctx) { ((fake_t *) ctx)->hashed = 0; return ESP_OK; }
static esp_err_t hash_update(const uint8_t *p, size_t n, void *ctx)
{ (void) p; ((fake_t *) ctx)->hashed += (uint32_t) n; return ESP_OK; }
static esp_err_t hash_finish(uint8_t *digest, void *ctx)
{ memset(digest, ((fake_t *) ctx)->bad_hash ? 1 : 0, 32); return ESP_OK; }

static firmware_manifest_t fixture(fake_t *f)
{
    memset(f, 0, sizeof(*f)); f->length = 512;
    firmware_manifest_t manifest = {.identity = {FIRMWARE_IDENTITY_MAGIC, FIRMWARE_IDENTITY_FORMAT,
        FIRMWARE_PRODUCT, FIRMWARE_HARDWARE, FIRMWARE_LAYOUT, "1.0.1", 0}, .size = 512};
    memcpy(f->bytes + FIRMWARE_IDENTITY_OFFSET, &manifest.identity, sizeof(manifest.identity));
    return manifest;
}
static esp_err_t run(fake_t *f, const firmware_manifest_t *manifest)
{
    const firmware_stream_ops_t ops = {.ctx = f, .now_ms = tick, .read = read_bytes,
        .begin = begin, .write = write_bytes, .finish = finish, .abort = abort_transfer,
        .hash_begin = hash_begin, .hash_update = hash_update, .hash_finish = hash_finish};
    return firmware_transfer_stage(manifest, &ops, NULL, NULL);
}
int main(void)
{
    fake_t f; firmware_manifest_t manifest = fixture(&f);
    assert(run(&f, &manifest) == ESP_OK);
    assert(f.written == 512 && f.hashed == 512 && f.finish_calls == 1 && f.abort_calls == 0);
    manifest = fixture(&f); f.length = 511;
    assert(run(&f, &manifest) != ESP_OK && f.abort_calls == 1 && !f.finish_calls);
    manifest = fixture(&f); f.length = 513;
    assert(run(&f, &manifest) != ESP_OK && f.abort_calls == 1 && !f.finish_calls);
    manifest = fixture(&f); f.bad_hash = true;
    assert(run(&f, &manifest) != ESP_OK && f.abort_calls == 1 && !f.finish_calls);
    manifest = fixture(&f); f.bytes[FIRMWARE_IDENTITY_OFFSET + 32] = 'x';
    assert(run(&f, &manifest) != ESP_OK && f.abort_calls == 1 && !f.finish_calls);
    manifest = fixture(&f); f.read_failure = true;
    assert(run(&f, &manifest) != ESP_OK && f.abort_calls == 1 && !f.finish_calls);
    manifest = fixture(&f); f.finish_failure = true;
    assert(run(&f, &manifest) != ESP_OK && f.finish_calls == 1 && !f.abort_calls);
    manifest = fixture(&f); f.expired = true;
    assert(run(&f, &manifest) == ESP_ERR_TIMEOUT && f.abort_calls == 1 && !f.finish_calls);
    puts("firmware transfer passed");
    return 0;
}
