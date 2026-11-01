#pragma once
#include "esp_err.h"
#include "firmware_types.h"

typedef struct { char ssid[33], password[64]; } device_wifi_credentials_t;
esp_err_t device_config_init(void);
esp_err_t device_config_load_wifi(device_wifi_credentials_t *out);
esp_err_t device_config_seed_wifi(const device_wifi_credentials_t *seed);
esp_err_t device_config_save_wifi(const device_wifi_credentials_t *credentials);
esp_err_t device_config_load_attempt(firmware_attempt_t *out);
esp_err_t device_config_save_attempt(const firmware_attempt_t *attempt);
esp_err_t device_config_bootstrap_confirmed(void);
esp_err_t device_config_confirm_bootstrap(void);
