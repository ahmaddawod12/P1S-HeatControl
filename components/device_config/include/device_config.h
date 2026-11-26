#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "firmware_types.h"
#include "printer_types.h"
#include "device_name.h"

typedef struct {
  char ssid[33], password[64];
  char controller_name[DEVICE_NAME_LEN];
} device_wifi_credentials_t;

#define DEVICE_FIREBASE_DATABASE_BYTES 262U
#define DEVICE_FIREBASE_UID_BYTES 129U
#define DEVICE_FIREBASE_REFRESH_BYTES 1024U
#define DEVICE_FIREBASE_RECOVERY_KEY_BYTES 65U
#define DEVICE_SETUP_PASSPHRASE_BYTES 33U

typedef struct {
  char database_url[DEVICE_FIREBASE_DATABASE_BYTES];
  char uid[DEVICE_FIREBASE_UID_BYTES];
  char refresh_token[DEVICE_FIREBASE_REFRESH_BYTES];
  bool pairing_pending;
  char recovery_key[DEVICE_FIREBASE_RECOVERY_KEY_BYTES];
  bool recovery_pending;
} device_firebase_session_t;

esp_err_t device_config_init(void);
esp_err_t device_config_load_firebase(device_firebase_session_t *out);
esp_err_t device_config_save_firebase(const device_firebase_session_t *session);
esp_err_t device_config_load_wifi(device_wifi_credentials_t *out);
esp_err_t device_config_seed_wifi(const device_wifi_credentials_t *seed);
esp_err_t device_config_save_wifi(const device_wifi_credentials_t *credentials);
esp_err_t device_config_load_setup_passphrase(char out[DEVICE_SETUP_PASSPHRASE_BYTES]);
esp_err_t device_config_save_setup_passphrase(const char passphrase[DEVICE_SETUP_PASSPHRASE_BYTES]);
esp_err_t device_config_load_attempt(firmware_attempt_t *out);
esp_err_t device_config_save_attempt(const firmware_attempt_t *attempt);
/* NULL selects the legacy LAN marker. A pilot supplies its ELF SHA-256. */
#define DEVICE_BOOTSTRAP_HASH_BYTES 32U
esp_err_t device_config_bootstrap_confirmed(const uint8_t *pilot_image_hash);
esp_err_t device_config_confirm_bootstrap(const uint8_t *pilot_image_hash);
bool printer_config_valid(const printer_connection_config_t *config);
esp_err_t device_config_load_printer(printer_connection_config_t *out);
esp_err_t device_config_save_printer(const printer_connection_config_t *config);
esp_err_t device_config_clear_printer(void);
