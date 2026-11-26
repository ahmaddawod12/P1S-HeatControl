#include "firmware_types.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"

#ifdef CONFIG_P1S_FIREBASE_PUBLIC_RELEASE
#define IMAGE_PROFILE_FLAG 0U
#elif defined(CONFIG_P1S_FIREBASE_ENABLED)
#define IMAGE_PROFILE_FLAG FIRMWARE_IMAGE_FIREBASE_PILOT
#else
/* An unconfigured image cannot be installed as a public OTA release. */
#define IMAGE_PROFILE_FLAG FIRMWARE_IMAGE_NO_FIREBASE
#endif

const __attribute__((section(".rodata_custom_desc"), used))
firmware_identity_t custom_app_desc = {
    FIRMWARE_IDENTITY_MAGIC, FIRMWARE_IDENTITY_FORMAT, FIRMWARE_PRODUCT, FIRMWARE_HARDWARE,
    FIRMWARE_LAYOUT, P1S_FIRMWARE_VERSION,
    IMAGE_PROFILE_FLAG | (P1S_PERSONAL_BOOTSTRAP ? FIRMWARE_IMAGE_PERSONAL : 0U) |
    ((sizeof(CONFIG_P1S_WIFI_SSID) > 1 || sizeof(CONFIG_P1S_WIFI_PASSWORD) > 1)
        ? FIRMWARE_IMAGE_CREDENTIALS : 0U),
};
