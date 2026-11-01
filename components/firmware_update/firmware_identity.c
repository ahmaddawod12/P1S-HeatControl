#include "firmware_types.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"

const __attribute__((section(".rodata_custom_desc"), used))
firmware_identity_t custom_app_desc = {
    FIRMWARE_IDENTITY_MAGIC, FIRMWARE_IDENTITY_FORMAT, FIRMWARE_PRODUCT, FIRMWARE_HARDWARE,
    FIRMWARE_LAYOUT, P1S_FIRMWARE_VERSION,
    (P1S_PERSONAL_BOOTSTRAP ? FIRMWARE_IMAGE_PERSONAL : 0U) |
    ((sizeof(CONFIG_P1S_WIFI_SSID) > 1 || sizeof(CONFIG_P1S_WIFI_PASSWORD) > 1)
        ? FIRMWARE_IMAGE_CREDENTIALS : 0U),
};
