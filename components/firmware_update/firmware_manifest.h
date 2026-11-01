#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "firmware_types.h"
#define FIRMWARE_URL_BYTES 2049U
#define FIRMWARE_RELEASE_JSON_MAX 32768U
#define FIRMWARE_MANIFEST_JSON_MAX 2048U
typedef struct {
    char version[32], tag[32];
    char manifest_url[FIRMWARE_URL_BYTES], image_url[FIRMWARE_URL_BYTES];
} firmware_release_t;
typedef struct { firmware_release_t release; firmware_manifest_t manifest; } firmware_candidate_t;
esp_err_t firmware_release_parse(const char *json, size_t length, firmware_release_t *out);
esp_err_t firmware_manifest_parse(const char *json, size_t length, firmware_manifest_t *out);
