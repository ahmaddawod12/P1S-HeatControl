#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FIRMWARE_IDENTITY_MAGIC UINT32_C(0x31533150)
#define FIRMWARE_IDENTITY_FORMAT 2U
#define FIRMWARE_IMAGE_PERSONAL 1U
#define FIRMWARE_IMAGE_CREDENTIALS 2U
#define FIRMWARE_PRODUCT "p1s-heat-controller"
#define FIRMWARE_HARDWARE "esp32-wroom-32"
#define FIRMWARE_LAYOUT "p1s-4m-ota-v1"
#define FIRMWARE_IMAGE_NAME "p1s-heat-controller-esp32-4mb.bin"
#define FIRMWARE_SLOT_BYTES UINT32_C(0x1F0000)
#define FIRMWARE_VERSION_BYTES 32U

typedef struct { uint32_t major, minor, patch; } firmware_version_t;
typedef struct {
    uint32_t magic, format;
    char product[24], hardware[24], layout[24], version[32];
    uint32_t image_flags;
} firmware_identity_t;
_Static_assert(sizeof(firmware_identity_t) == 116, "firmware identity is a wire format");

typedef enum {
    FIRMWARE_IDLE = 0, FIRMWARE_CHECKING = 1, FIRMWARE_COOLING = 2,
    FIRMWARE_DOWNLOADING = 3, FIRMWARE_VERIFYING = 4, FIRMWARE_REBOOTING = 5,
    FIRMWARE_TRIAL = 6, FIRMWARE_SUCCEEDED = 7, FIRMWARE_FAILED = 8,
} firmware_phase_t;
typedef enum {
    FIRMWARE_RESULT_NONE = 0, FIRMWARE_RESULT_UP_TO_DATE = 1,
    FIRMWARE_RESULT_NO_RELEASE = 2, FIRMWARE_RESULT_INSTALLED = 3,
    FIRMWARE_RESULT_CHECK_FAILED = 4, FIRMWARE_RESULT_UNSAFE = 5,
    FIRMWARE_RESULT_DOWNLOAD_FAILED = 6, FIRMWARE_RESULT_INVALID_IMAGE = 7,
    FIRMWARE_RESULT_STORAGE_FAILED = 8, FIRMWARE_RESULT_ROLLED_BACK = 9,
    FIRMWARE_RESULT_TRIAL_FAILED = 10, FIRMWARE_RESULT_SETUP_REQUIRED = 11,
    FIRMWARE_RESULT_INTERRUPTED = 12,
} firmware_result_t;
typedef enum {
    FIRMWARE_CHECK_AVAILABLE = 0, FIRMWARE_CHECK_UP_TO_DATE = 1,
    FIRMWARE_CHECK_NO_RELEASE = 2,
} firmware_check_result_t;
typedef struct {
    firmware_identity_t identity;
    char filename[64];
    uint32_t size;
    uint8_t sha256[32];
} firmware_manifest_t;
typedef struct {
    uint32_t format, id;
    char previous[32], candidate[32];
    firmware_phase_t phase;
    firmware_result_t result;
} firmware_attempt_t;
typedef struct {
    char installed[32], candidate[32];
    firmware_phase_t phase;
    firmware_result_t result;
    uint32_t received, total;
    bool maintenance, trial_pending;
} firmware_status_t;
