#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"
#include "esp_err.h"
#include "temperature_sensor.h"
#include "runtime_auto_types.h"

/* Injection boundary for the sensor, relay outputs, and setting
 * persistence. Tests supply fakes; production wiring lives in
 * app_runtime_start(). */
typedef struct {
    esp_err_t (*read_sensor)(temperature_sensor_reading_t *out_reading, void *ctx);
    esp_err_t (*set_outputs)(bool heater_on, bool fan_on, void *ctx);
    esp_err_t (*save_target)(float target_c, void *ctx);
    esp_err_t (*save_calibration)(float calibration_offset_c, void *ctx);
    void *ctx;
} app_runtime_ops_t;

/* Builds the production operations and delegates to
 * app_runtime_start_with_ops(). */
esp_err_t app_runtime_start(void);
esp_err_t app_runtime_start_with_ops(const app_runtime_ops_t *ops);
void app_runtime_stop(void);
esp_err_t app_runtime_set_boot_maintenance(bool enabled);
esp_err_t app_runtime_begin_maintenance(uint32_t timeout_ms);
esp_err_t app_runtime_end_maintenance(void);
bool app_runtime_maintenance_active(void);

/* Latches HEAT_FAULT_NVS or HEAT_FAULT_OUTPUT_INIT raised before or during
 * startup. Heating stays off until the fault is acknowledged. */
esp_err_t app_runtime_report_startup_fault(heat_fault_t fault);

esp_err_t app_runtime_set_target(float target_c);
esp_err_t app_runtime_set_calibration(float calibration_offset_c);
esp_err_t app_runtime_set_enabled(bool enabled);
esp_err_t app_runtime_set_manual_fan(bool enabled);
/* Wait up to 500 ms for the controller's decision. Refusal returns
 * ESP_ERR_NOT_ALLOWED; a request expired before execution is discarded. */
esp_err_t app_runtime_ack_fault(void);

void app_runtime_set_network_status(int8_t rssi, const char *ip_addr);
void app_runtime_get_status(app_status_t *out_status);
esp_err_t app_runtime_auto_acquire(uint64_t cycle_id,float target_c,uint64_t expires_ms);
esp_err_t app_runtime_auto_renew(uint64_t cycle_id,uint64_t expires_ms);
void app_runtime_auto_revoke(uint64_t cycle_id);
void app_runtime_get_auto_status(runtime_auto_status_t *out);
