#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "device_config.h"

#define WIFI_MANAGER_SSID_LEN 33U
#define WIFI_MANAGER_PASSWORD_LEN 64U
#define WIFI_MANAGER_IP_LEN 16U
#define WIFI_MANAGER_BOARD_ID_LEN 13U
#define WIFI_MANAGER_AP_SSID_PREFIX "P1S-Heat-Setup"
#define WIFI_MANAGER_AP_IP "192.168.4.1"

typedef enum {
    WIFI_RESULT_NONE, WIFI_RESULT_SETUP_REQUIRED, WIFI_RESULT_CONNECTING,
    WIFI_RESULT_CONNECTED, WIFI_RESULT_NO_NETWORK, WIFI_RESULT_AUTH_FAILED,
    WIFI_RESULT_SAVE_FAILED, WIFI_RESULT_AP_FAILED, WIFI_RESULT_DRIVER_FAILED,
    WIFI_RESULT_STORAGE_FAILED
} wifi_result_t;

typedef enum {
    WIFI_SAVE_NONE, WIFI_SAVE_PENDING, WIFI_SAVE_SAVED, WIFI_SAVE_FAILED, WIFI_SAVE_AP_FAILED,
} wifi_save_result_t;

typedef enum {
    WIFI_SETUP_NONE, WIFI_SETUP_NETWORK, WIFI_SETUP_PAIRING,
} wifi_setup_mode_t;

typedef struct {
    bool connected;
    char ssid[WIFI_MANAGER_SSID_LEN];
    char ip_addr[WIFI_MANAGER_IP_LEN];
    int8_t rssi;
    char controller_name[DEVICE_NAME_LEN];
    char board_id[WIFI_MANAGER_BOARD_ID_LEN];
    bool configured, ap_active, applying, can_save;
    wifi_setup_mode_t setup_mode;
    bool setup_portal_ready;
    wifi_result_t result;
    uint32_t save_id;
    wifi_save_result_t save_result;
} wifi_manager_status_t;

typedef device_wifi_credentials_t wifi_credentials_t;

/* Both forms reject null pointers, empty or over-long SSIDs, and passwords
 * that are neither empty nor 8 to 63 bytes. The `_n` form additionally
 * rejects fixed-size fields that arrive without a terminator. */
bool wifi_credentials_are_valid(const char *ssid, const char *password);
bool wifi_credentials_are_valid_n(const char *ssid,
                                  size_t ssid_capacity,
                                  const char *password,
                                  size_t password_capacity);

/* Copies a validated pair into `out_credentials`, which is left untouched
 * when the pair is rejected. This is the only way credentials enter the
 * manager from persisted configuration or a personal setup seed. */
esp_err_t wifi_credentials_init(wifi_credentials_t *out_credentials,
                                const char *ssid,
                                const char *password);

/* Delay before reconnect attempt `attempt`, counting from zero: 1, 2, 4, 8,
 * then 30 seconds for every attempt after that. Retries never give up. */
uint32_t wifi_manager_backoff_ms(size_t attempt);

/* Joins the saved network. A personal setup build seeds an absent record
 * once; generic updates never replace a saved record. */
esp_err_t wifi_manager_start(void);
void wifi_manager_get_status(wifi_manager_status_t *out_status);
/* Returns the station connection generation when the link is available.
 * Disconnect events and accepted credential changes invalidate older generations. */
bool wifi_manager_connection_generation(uint32_t *generation);
/* Serialize a short relay commit with disconnect reporting and credential changes.
 * Release the connection lock after a successful call. */
bool wifi_manager_lock_connection(uint32_t *generation);
void wifi_manager_unlock_connection(void);
/* Enter credentials during Wi-Fi setup and return the save receipt ID. */
esp_err_t wifi_manager_request_credentials(const wifi_credentials_t *credentials, uint32_t *save_id);
const char *wifi_manager_result_name(wifi_result_t result);

/* The Wi-Fi worker owns the hotspot and calls these hooks while it is active. */
typedef esp_err_t (*wifi_setup_portal_fn)(void);
void wifi_manager_set_setup_portal(wifi_setup_portal_fn start, wifi_setup_portal_fn stop);
/* A confirmed enrollment stays paired during temporary Firebase failures. */
void wifi_manager_set_paired(bool paired);
