#include "firmware_policy.h"

#include <stddef.h>
#include <string.h>

bool firmware_version_parse(const char *text, firmware_version_t *out)
{
    if (text == NULL || out == NULL) return false;
    size_t length = 0;
    while (length < 32 && text[length] != '\0') ++length;
    if (length == 0 || length >= 32) return false;
    if (*text == 'v') ++text;
    uint32_t values[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        const char *start = text;
        if (*text < '0' || *text > '9') return false;
        do {
            const unsigned digit = (unsigned) (*text++ - '0');
            if (values[i] > (UINT32_MAX - digit) / 10U) return false;
            values[i] = values[i] * 10U + digit;
        } while (*text >= '0' && *text <= '9');
        if (text - start > 1 && *start == '0') return false;
        if (i < 2) { if (*text++ != '.') return false; }
        else if (*text != '\0') return false;
    }
    *out = (firmware_version_t) {values[0], values[1], values[2]};
    return true;
}

int firmware_version_compare(const firmware_version_t *a, const firmware_version_t *b)
{
    if (a->major != b->major) return a->major > b->major ? 1 : -1;
    if (a->minor != b->minor) return a->minor > b->minor ? 1 : -1;
    return (a->patch > b->patch) - (a->patch < b->patch);
}

bool firmware_identity_valid(const firmware_identity_t *identity)
{
    firmware_version_t version;
    return identity != NULL && identity->magic == FIRMWARE_IDENTITY_MAGIC &&
           identity->format == FIRMWARE_IDENTITY_FORMAT && identity->image_flags == 0 &&
           memchr(identity->product, 0, sizeof(identity->product)) != NULL &&
           memchr(identity->hardware, 0, sizeof(identity->hardware)) != NULL &&
           memchr(identity->layout, 0, sizeof(identity->layout)) != NULL &&
           memchr(identity->version, 0, sizeof(identity->version)) != NULL &&
           strcmp(identity->product, FIRMWARE_PRODUCT) == 0 &&
           strcmp(identity->hardware, FIRMWARE_HARDWARE) == 0 &&
           strcmp(identity->layout, FIRMWARE_LAYOUT) == 0 &&
           identity->version[0] != 'v' && firmware_version_parse(identity->version, &version);
}

bool firmware_identity_matches(const firmware_identity_t *a, const firmware_identity_t *b)
{
    return firmware_identity_valid(a) && firmware_identity_valid(b) &&
           strcmp(a->version, b->version) == 0;
}

const char *firmware_phase_name(firmware_phase_t phase)
{
    static const char *const names[] = {"idle", "checking", "cooling", "downloading",
        "verifying", "rebooting", "trial", "succeeded", "failed"};
    return (unsigned) phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "failed";
}

const char *firmware_result_name(firmware_result_t result)
{
    static const char *const names[] = {"none", "up_to_date", "no_release", "installed",
        "check_failed", "unsafe", "download_failed", "invalid_image", "storage_failed",
        "rolled_back", "trial_failed", "setup_required", "interrupted"};
    return (unsigned) result < sizeof(names) / sizeof(names[0]) ? names[result] : "check_failed";
}
