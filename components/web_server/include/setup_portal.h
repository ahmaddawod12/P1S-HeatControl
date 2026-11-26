#pragma once

#include "esp_err.h"

#define SETUP_PAIRING_CODE_BYTES 17U
typedef enum {
    SETUP_PAIRING_CODE,
    SETUP_PAIRING_USB_REQUIRED,
    SETUP_PAIRING_ACCOUNT_DISABLED,
} setup_pairing_state_t;
typedef setup_pairing_state_t (*setup_pairing_code_fn)(char code[SETUP_PAIRING_CODE_BYTES]);

esp_err_t setup_portal_start(setup_pairing_code_fn get_code);
esp_err_t setup_portal_stop(void);
