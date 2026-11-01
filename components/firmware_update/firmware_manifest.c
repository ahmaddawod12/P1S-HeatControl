#include "firmware_manifest.h"
#include "firmware_policy.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static cJSON *parse_object(const char *json, size_t length, size_t maximum)
{
    if (!json || !length || length > maximum || memchr(json, 0, length)) return NULL;
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, json, length); copy[length] = 0;
    cJSON *root = strstr(copy, "\\u0000") ? NULL : cJSON_ParseWithLengthOpts(copy, length + 1, NULL, true);
    free(copy);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    for (const cJSON *a = root->child; a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next)
            if (a->string && b->string && strcmp(a->string, b->string) == 0) {
                cJSON_Delete(root); return NULL;
            }
    return root;
}

static bool copy_string(const cJSON *root, const char *key, char *out, size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(value) || !value->valuestring || strlen(value->valuestring) >= capacity) return false;
    strcpy(out, value->valuestring);
    return true;
}
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

esp_err_t firmware_manifest_parse(const char *json, size_t length, firmware_manifest_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    cJSON *root = parse_object(json, length, FIRMWARE_MANIFEST_JSON_MAX);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    firmware_manifest_t result = {.identity = {.magic = FIRMWARE_IDENTITY_MAGIC, .format = FIRMWARE_IDENTITY_FORMAT}};
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "format");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    char digest[65];
    bool valid = cJSON_GetArraySize(root) == 8 && cJSON_IsNumber(format) && format->valuedouble == 1 &&
        copy_string(root, "product", result.identity.product, sizeof(result.identity.product)) &&
        copy_string(root, "hardware", result.identity.hardware, sizeof(result.identity.hardware)) &&
        copy_string(root, "layout", result.identity.layout, sizeof(result.identity.layout)) &&
        copy_string(root, "version", result.identity.version, sizeof(result.identity.version)) &&
        copy_string(root, "filename", result.filename, sizeof(result.filename)) &&
        strcmp(result.filename, FIRMWARE_IMAGE_NAME) == 0 &&
        firmware_identity_valid(&result.identity) && cJSON_IsNumber(size) &&
        size->valuedouble >= 400 && size->valuedouble <= FIRMWARE_SLOT_BYTES &&
        size->valuedouble == (double) (uint32_t) size->valuedouble &&
        copy_string(root, "sha256", digest, sizeof(digest)) && strlen(digest) == 64;
    if (valid) {
        result.size = (uint32_t) size->valuedouble;
        for (unsigned i = 0; i < 32; ++i) {
            const int a = hex_digit(digest[2 * i]), b = hex_digit(digest[2 * i + 1]);
            if (a < 0 || b < 0) { valid = false; break; }
            result.sha256[i] = (uint8_t) ((a << 4) | b);
        }
    }
    cJSON_Delete(root);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;
    *out = result;
    return ESP_OK;
}

esp_err_t firmware_release_parse(const char *json, size_t length, firmware_release_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    cJSON *root = parse_object(json, length, FIRMWARE_RELEASE_JSON_MAX);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    firmware_release_t *result = calloc(1, sizeof(*result));
    if (!result) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    firmware_version_t version;
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    bool valid = copy_string(root, "tag_name", result->tag, sizeof(result->tag)) &&
        firmware_version_parse(result->tag, &version) &&
        cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "draft")) &&
        cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "prerelease")) && cJSON_IsArray(assets);
    if (valid) strcpy(result->version, result->tag + (result->tag[0] == 'v'));
    unsigned manifests = 0, images = 0;
    for (const cJSON *asset = valid ? assets->child : NULL; asset; asset = asset->next) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        if (!cJSON_IsObject(asset) || !cJSON_IsString(name)) { valid = false; break; }
        char *destination = NULL;
        if (strcmp(name->valuestring, "firmware.json") == 0) { ++manifests; destination = result->manifest_url; }
        if (strcmp(name->valuestring, FIRMWARE_IMAGE_NAME) == 0) { ++images; destination = result->image_url; }
        if (destination && (!copy_string(asset, "browser_download_url", destination, FIRMWARE_URL_BYTES) ||
            strncmp(destination, "https://", 8) != 0 || strpbrk(destination, "\r\n\t ") != NULL)) valid = false;
    }
    valid = valid && manifests == 1 && images == 1;
    if (valid) *out = *result;
    free(result); cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
