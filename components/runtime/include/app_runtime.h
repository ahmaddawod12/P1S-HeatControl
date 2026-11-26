#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"
#include "esp_err.h"
#include "printer_types.h"
#include "temperature_sensor.h"

/* Injection boundary for the sensor, relay outputs, and setting
 * persistence. Tests supply fakes; production wiring lives in
 * app_runtime_start(). */
typedef struct {
    esp_err_t (*read_sensor)(temperature_sensor_reading_t *out_reading, void *ctx);
    esp_err_t (*set_relay)(bool on, void *ctx);
    esp_err_t (*save_target)(float target_c, void *ctx);
    esp_err_t (*save_calibration)(float calibration_offset_c, void *ctx);
    void *ctx;
    heat_mode_t saved_heat_mode;
    esp_err_t (*save_heat_mode)(heat_mode_t mode);
    esp_err_t (*save_hysteresis)(float hysteresis_c, void *ctx);
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
/* The relay is known off and all admitted commands and safety writes have settled. */
bool app_runtime_maintenance_ready(void);

/* Latches HEAT_FAULT_NVS or HEAT_FAULT_OUTPUT_INIT raised before or during
 * startup. Heating stays off until the fault is acknowledged. */
esp_err_t app_runtime_report_startup_fault(heat_fault_t fault);

esp_err_t app_runtime_set_target(float target_c);
esp_err_t app_runtime_set_calibration(float calibration_offset_c);
esp_err_t app_runtime_set_enabled(bool enabled);
/* Wait up to 500 ms to confirm the fault and any stored lockout were cleared.
 * Refusal returns ESP_ERR_NOT_ALLOWED. An expired request cannot start. */
esp_err_t app_runtime_ack_fault(void);

/* Remote requests use a monotonic admission deadline and confirm their result. */
esp_err_t app_runtime_set_target_until(float target_c, uint64_t deadline_ms);
esp_err_t app_runtime_set_restart_until(float restart_c, uint64_t deadline_ms);
esp_err_t app_runtime_set_calibration_until(float offset_c, uint64_t deadline_ms);
esp_err_t app_runtime_set_enabled_until(bool enabled, uint64_t deadline_ms);
esp_err_t app_runtime_ack_fault_until(uint64_t deadline_ms);

/* The guard serializes authorization changes with enabling heat or releasing
 * a monitor hold. It must remain valid after the caller stops waiting.
 * A successful lock_if_current() holds it until unlock(). Neither function
 * may call back into runtime. */
typedef struct app_runtime_enable_guard {
    bool (*lock_if_current)(uint32_t revision);
    void (*unlock)(void);
    /* For relay starts outside the command that first enabled heat. This
     * interlock checks for pending Disable even when Wi-Fi is unavailable. */
    bool (*lock_relay_start)(void);
    void (*unlock_relay_start)(void);
} app_runtime_enable_guard_t;
/* The guard must have static lifetime. Register before receiving commands. */
void app_runtime_set_relay_start_guard(const app_runtime_enable_guard_t *guard);
esp_err_t app_runtime_enable_if_current(uint64_t deadline_ms,
                                        const app_runtime_enable_guard_t *guard, uint32_t revision);

void app_runtime_set_network_status(int8_t rssi, const char *ip_addr);
void app_runtime_get_status(app_status_t *out_status);
/* While monitoring, the relay may run only while the latest permit is current.
 * Recovered sessions start with no permit. Disabling heating ends the session. */
#define APP_RUNTIME_MONITOR_PERMIT_MAX_MS 5000U
esp_err_t app_runtime_monitor_begin(printer_monitor_phase_t phase,
                                    uint64_t permit_until_ms);
esp_err_t app_runtime_monitor_begin_until(printer_monitor_phase_t phase,
                                         uint64_t permit_until_ms, uint64_t deadline_ms);
esp_err_t app_runtime_monitor_permit(printer_monitor_phase_t phase,
                                     uint64_t permit_until_ms);
/* Clear recovery before a printer settings change. A later permit saves the
 * phase again if the existing session continues. */
esp_err_t app_runtime_monitor_suspend_recovery(void);
/* An already-ended session leaves the current heating mode alone. */
esp_err_t app_runtime_monitor_end(bool disable_heating);
/* An optional guard authorizes the switch back to manual heating. */
esp_err_t app_runtime_monitor_end_until(bool disable_heating, uint64_t deadline_ms,
                                       const app_runtime_enable_guard_t *guard, uint32_t revision);
printer_monitor_phase_t app_runtime_monitor_phase(void);
bool app_runtime_monitor_active(void);
