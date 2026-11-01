#pragma once
#include "esp_err.h"
#include "firmware_types.h"
extern const firmware_identity_t custom_app_desc;
esp_err_t firmware_update_start(void);
esp_err_t firmware_update_request(void);
void firmware_update_get_status(firmware_status_t *out);
esp_err_t firmware_trial_start(void);
bool firmware_trial_pending(void);
void firmware_trial_startup_progress(void);
void firmware_trial_startup_complete(esp_err_t result);
void firmware_update_note_boot_result(firmware_result_t result);
