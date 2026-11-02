#include "firmware_transport.h"
#include "firmware_policy.h"
#include "app_runtime.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "clock_service.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

_Static_assert(FIRMWARE_IDENTITY_OFFSET == sizeof(esp_image_header_t) +
    sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t), "SDK image layout changed");

typedef struct {
    esp_http_client_handle_t client;
    uint64_t deadline;
    char url[FIRMWARE_URL_BYTES], location[FIRMWARE_URL_BYTES];
    bool bad_header;
} http_stream_t;

static uint64_t monotonic_ms(void) { return (uint64_t) (esp_timer_get_time() / 1000); }
static bool https_url(const char *url)
{ return url && strncmp(url, "https://", 8) == 0 && strlen(url) < FIRMWARE_URL_BYTES && !strpbrk(url, "\r\n\t "); }
static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_stream_t *stream = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        strcasecmp(event->header_key, "Location") == 0) {
        if (!event->header_value || strlen(event->header_value) >= sizeof(stream->location)) stream->bad_header = true;
        else strcpy(stream->location, event->header_value);
    }
    return ESP_OK;
}
static void http_close(http_stream_t *stream)
{
    if (!stream) return;
    if (stream->client) esp_http_client_cleanup(stream->client);
    free(stream);
}
static esp_err_t http_open(const char *url, uint64_t deadline, http_stream_t **out, int *status)
{
    if (!https_url(url)) return ESP_ERR_INVALID_ARG;
    http_stream_t *stream = calloc(1, sizeof(*stream));
    if (!stream) return ESP_ERR_NO_MEM;
    strcpy(stream->url, url); stream->deadline = deadline;
    esp_err_t err = ESP_FAIL;
    for (unsigned redirects = 0; redirects <= 5; ++redirects) {
        if (monotonic_ms() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
        stream->location[0] = 0; stream->bad_header = false;
        const esp_http_client_config_t config = {.url = stream->url,
            .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true, .event_handler = http_event, .user_data = stream,
            .user_agent = "P1S-HeatControl-OTA", .buffer_size = 4096,
            /* GitHub's signed asset query exceeds IDF's 512-byte TX default. */
            .buffer_size_tx = 4096};
        stream->client = esp_http_client_init(&config);
        if (!stream->client) { err = ESP_ERR_NO_MEM; break; }
        err = esp_http_client_open(stream->client, 0);
        if (err != ESP_OK) break;
        const int64_t headers = esp_http_client_fetch_headers(stream->client);
        *status = esp_http_client_get_status_code(stream->client);
        if (headers < 0 || stream->bad_header) { err = ESP_ERR_INVALID_RESPONSE; break; }
        if (*status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308) {
            if (redirects == 5 || !https_url(stream->location)) { err = ESP_ERR_INVALID_RESPONSE; break; }
            strcpy(stream->url, stream->location);
            esp_http_client_cleanup(stream->client); stream->client = NULL;
            continue;
        }
        if (monotonic_ms() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
        *out = stream; return ESP_OK;
    }
    http_close(stream);
    return err;
}
static int http_read(http_stream_t *stream, uint8_t *buffer, size_t capacity)
{
    if (monotonic_ms() >= stream->deadline) return -1;
    const int count = esp_http_client_read(stream->client, (char *) buffer, (int) capacity);
    if (count == 0 && !esp_http_client_is_complete_data_received(stream->client)) return -1;
    return count;
}
static esp_err_t fetch_json(const char *url, size_t maximum, uint64_t deadline, char **out, size_t *length, int *status)
{
    http_stream_t *stream = NULL;
    esp_err_t err = http_open(url, deadline, &stream, status);
    if (err != ESP_OK) return err;
    if (*status != 200) { http_close(stream); return ESP_ERR_INVALID_RESPONSE; }
    char *body = malloc(maximum + 1);
    if (!body) { http_close(stream); return ESP_ERR_NO_MEM; }
    size_t used = 0;
    while (true) {
        const int n = http_read(stream, (uint8_t *) body + used, maximum + 1 - used);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;
        used += (size_t) n;
        if (used > maximum) { err = ESP_ERR_INVALID_SIZE; break; }
    }
    http_close(stream);
    if (err != ESP_OK) { free(body); return err; }
    body[used] = 0; *out = body; *length = used;
    return ESP_OK;
}
static esp_err_t clock_ready(void)
{
    return clock_service_wait("pool.ntp.org", 30000, NULL, NULL);
}

esp_err_t firmware_transport_check(const char *installed, firmware_candidate_t *out,
    firmware_check_result_t *out_result)
{
    firmware_version_t current, candidate;
    if (!out || !out_result || !firmware_version_parse(installed, &current)) return ESP_ERR_INVALID_ARG;
    const uint64_t deadline = monotonic_ms() + 60000;
    esp_err_t err = clock_ready();
    if (err != ESP_OK) return err;
    char url[256];
    if (snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", CONFIG_P1S_RELEASE_REPOSITORY) >= (int) sizeof(url)) return ESP_ERR_INVALID_SIZE;
    char *json = NULL; size_t length = 0; int status = 0;
    err = fetch_json(url, FIRMWARE_RELEASE_JSON_MAX, deadline, &json, &length, &status);
    if (status == 404) { *out_result = FIRMWARE_CHECK_NO_RELEASE; return ESP_OK; }
    if (err != ESP_OK) return err;
    err = firmware_release_parse(json, length, &out->release); free(json);
    if (err != ESP_OK) return err;
    if (!firmware_version_parse(out->release.version, &candidate)) return ESP_ERR_INVALID_RESPONSE;
    if (firmware_version_compare(&candidate, &current) <= 0) { *out_result = FIRMWARE_CHECK_UP_TO_DATE; return ESP_OK; }
    char expected[384];
    snprintf(expected, sizeof(expected), "https://github.com/%s/releases/download/%s/firmware.json", CONFIG_P1S_RELEASE_REPOSITORY, out->release.tag);
    if (strcmp(expected, out->release.manifest_url) != 0) return ESP_ERR_INVALID_RESPONSE;
    snprintf(expected, sizeof(expected), "https://github.com/%s/releases/download/%s/%s", CONFIG_P1S_RELEASE_REPOSITORY, out->release.tag, FIRMWARE_IMAGE_NAME);
    if (strcmp(expected, out->release.image_url) != 0) return ESP_ERR_INVALID_RESPONSE;
    json = NULL;
    err = fetch_json(out->release.manifest_url, FIRMWARE_MANIFEST_JSON_MAX, deadline, &json, &length, &status);
    if (err != ESP_OK) return err;
    err = firmware_manifest_parse(json, length, &out->manifest); free(json);
    if (err != ESP_OK || strcmp(out->manifest.identity.version, out->release.version) != 0) return ESP_ERR_INVALID_RESPONSE;
    *out_result = FIRMWARE_CHECK_AVAILABLE;
    return ESP_OK;
}

typedef struct {
    http_stream_t *stream;
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    psa_hash_operation_t hash;
} native_stage_t;
static uint64_t native_now(void *ctx) { (void) ctx; return monotonic_ms(); }
static int native_read(uint8_t *p, size_t n, void *ctx) { return http_read(((native_stage_t *) ctx)->stream, p, n); }
static esp_err_t native_begin(uint32_t size, void *ctx)
{ native_stage_t *stage = ctx; return size <= stage->partition->size ? esp_ota_begin(stage->partition, size, &stage->handle) : ESP_ERR_INVALID_SIZE; }
static esp_err_t native_write(const uint8_t *p, size_t n, void *ctx)
{ return esp_ota_write(((native_stage_t *) ctx)->handle, p, n); }
static esp_err_t native_finish(void *ctx) { return esp_ota_end(((native_stage_t *) ctx)->handle); }
static void native_abort(void *ctx) { (void) esp_ota_abort(((native_stage_t *) ctx)->handle); }
static esp_err_t native_hash_begin(void *ctx)
{ return psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&((native_stage_t *) ctx)->hash, PSA_ALG_SHA_256) == PSA_SUCCESS ? ESP_OK : ESP_FAIL; }
static esp_err_t native_hash_update(const uint8_t *p, size_t n, void *ctx)
{ return psa_hash_update(&((native_stage_t *) ctx)->hash, p, n) == PSA_SUCCESS ? ESP_OK : ESP_FAIL; }
static esp_err_t native_hash_finish(uint8_t *out, void *ctx)
{ size_t size = 0; return psa_hash_finish(&((native_stage_t *) ctx)->hash, out, 32, &size) == PSA_SUCCESS && size == 32 ? ESP_OK : ESP_FAIL; }

esp_err_t firmware_transport_stage(const firmware_candidate_t *candidate,
    firmware_progress_cb_t progress, void *ctx, firmware_staged_t *out)
{
#if !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (!candidate || !out || !app_runtime_maintenance_active()) return ESP_ERR_NOT_ALLOWED;
    app_status_t status_snapshot; app_runtime_get_status(&status_snapshot);
    if (status_snapshot.enabled || status_snapshot.heater_on || status_snapshot.fan_on) return ESP_ERR_NOT_ALLOWED;
    native_stage_t stage = {.partition = esp_ota_get_next_update_partition(NULL)};
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!stage.partition || !running || stage.partition->address == running->address) return ESP_ERR_NOT_FOUND;
    int status = 0;
    esp_err_t err = http_open(candidate->release.image_url, monotonic_ms() + 180000, &stage.stream, &status);
    if (err != ESP_OK) return err;
    if (status != 200) { http_close(stage.stream); return ESP_ERR_INVALID_RESPONSE; }
    stage.hash = psa_hash_operation_init();
    const firmware_stream_ops_t ops = {.ctx = &stage, .now_ms = native_now, .read = native_read,
        .begin = native_begin, .write = native_write, .finish = native_finish, .abort = native_abort,
        .hash_begin = native_hash_begin, .hash_update = native_hash_update, .hash_finish = native_hash_finish};
    err = firmware_transfer_stage(&candidate->manifest, &ops, progress, ctx);
    (void) psa_hash_abort(&stage.hash); http_close(stage.stream);
    if (err == ESP_OK) *out = (firmware_staged_t) {.partition = stage.partition, .verified = true};
    return err;
}

esp_err_t firmware_transport_activate(const firmware_staged_t *staged)
{
    if (!staged || !staged->verified || !staged->partition || !app_runtime_maintenance_active()) return ESP_ERR_NOT_ALLOWED;
    return esp_ota_set_boot_partition(staged->partition);
}
