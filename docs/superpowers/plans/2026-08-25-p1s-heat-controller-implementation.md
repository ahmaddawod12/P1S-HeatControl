# P1S Heat Controller Implementation Plan

> **Sensor amendment — 2026-08-28:** The MAX31865/PT100 design recorded in
> Task 2 was subsequently replaced by the approved direct-ADC design: a
> 10 kΩ B3950 NTC from GPIO4 to ground, a 10 kΩ fixed resistor from 3.3 V to
> GPIO4, and a 100 nF capacitor from GPIO4 to ground. The current design spec,
> README, `components/thermistor/`, and thermistor tests supersede every
> sensor-specific instruction in this historical implementation plan. The
> controller, relay, persistence, networking, API, and UI tasks remain valid.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build ESP-IDF firmware for an ESP32-C3 SuperMini that reads a three-wire PT100 through a MAX31865, controls a 220 VAC / 350 W heater and 5 V fan through a two-channel relay board, and exposes a standalone local Wi-Fi dashboard.

**Architecture:** A temperature task owns the MAX31865 and publishes validated samples; a controller task owns the state machine and is the only code allowed to command the paired relay outputs. Wi-Fi, NVS settings, REST/SSE handlers, and the embedded web UI interact with the controller through typed commands and immutable status snapshots.

**Tech Stack:** ESP-IDF for target `esp32c3`, C17, FreeRTOS, `esp_driver_spi`, GPIO, NVS, `esp_wifi`, `esp_netif`, mDNS, `esp_http_server`, cJSON, Server-Sent Events, and ESP-IDF Unity tests.

**Spec:** `docs/superpowers/specs/2026-08-25-p1s-heat-controller-design.md`

## Global Constraints

- Target an ESP32-C3 SuperMini with ESP-IDF; do not add Arduino, PlatformIO, ESPHome, or cloud dependencies.
- Use GPIO3 for the heater relay, GPIO1 for the fan relay, and GPIO4/5/6/7 for MAX31865 MOSI/MISO/SCLK/CS.
- Treat the two independent relay channels as one logical output: both are on only in `HEAT_STATE_HEATING` and off in every other state.
- On activation, command the fan channel before the heater channel within the same control iteration; reverse that order on deactivation, with no intentional delay.
- Configure a three-wire PT100, `R0 = 100.0 ohms`, `RREF = 430.0 ohms`, IEC 60751 `alpha = 0.00385`, and MAX31865 50 Hz filtering.
- Sample every 500 ms, mark data stale after 2 seconds, and require three consecutive valid samples before enabling.
- Accept targets from 0 to 80 degrees Celsius in 0.5-degree increments; 0 disables heating, default target is 60, and hysteresis is fixed at 1 degree.
- Trip a latched over-temperature fault at 82 degrees Celsius from either raw or filtered temperature.
- Persist Wi-Fi credentials, target, and calibration offset; never persist enabled state, active fault, runtime state, or relay state.
- Start with both relays inactive on every boot and reset; heating never resumes automatically.
- Keep valid regulation running through Wi-Fi outages.
- Serve all UI assets locally; version 1 has no application password, OTA, PID, fan timing, Bambu integration, cloud service, or long-term history.
- Use only ESP-IDF built-in libraries and components. Pin ESP-IDF through the developer environment rather than adding third-party firmware dependencies.

---

## File Map

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | Top-level ESP-IDF project declaration |
| `sdkconfig.defaults` | ESP32-C3, USB console, HTTP, Wi-Fi, and partition defaults |
| `partitions.csv` | NVS and non-OTA factory application layout |
| `main/app_main.c` | Startup order and top-level service composition |
| `main/CMakeLists.txt` | Main component registration |
| `components/app_types/include/app_types.h` | Shared enums and immutable status/sample structures |
| `components/max31865/include/max31865.h` | MAX31865 public driver and conversion API |
| `components/max31865/max31865.c` | Register configuration, SPI reads, fault decoding, and PT100 conversion |
| `components/temperature/include/temperature_monitor.h` | Temperature filtering and health API |
| `components/temperature/temperature_monitor.c` | Median filtering, calibration, validity, and staleness |
| `components/controller/include/heat_controller.h` | Pure state-machine API |
| `components/controller/heat_controller.c` | Hysteresis, enabling, fault latching, and acknowledgement |
| `components/settings/include/settings.h` | Validated persistent-settings API |
| `components/settings/settings.c` | NVS schema, defaults, loading, and writes |
| `components/board/include/board_outputs.h` | Relay-output abstraction |
| `components/board/board_outputs.c` | Safe GPIO initialization and paired relay ordering |
| `components/board/Kconfig` | Build-time relay active-level selection |
| `components/runtime/include/app_runtime.h` | Thread-safe command and status interface used by the web layer |
| `components/runtime/app_runtime.c` | Temperature/controller tasks, queues, status mutex, and service ownership |
| `components/diagnostics/include/diagnostics.h` | Fixed-size diagnostic record API |
| `components/diagnostics/diagnostics.c` | 32-entry in-memory transition/fault ring buffer |
| `components/wifi_manager/include/wifi_manager.h` | Provisioning, station, scan, reset, and status API |
| `components/wifi_manager/wifi_manager.c` | ESP Wi-Fi event handling, NVS credentials, reconnect, and mDNS |
| `components/wifi_manager/captive_dns.c` | DNS redirect responder for provisioning mode |
| `components/web_server/include/web_server.h` | HTTP server lifecycle API |
| `components/web_server/include/api_contract.h` | JSON parsing/serialization boundary |
| `components/web_server/api_contract.c` | Target validation and status JSON |
| `components/web_server/sse_hub.c` | Bounded asynchronous SSE client registry and broadcasts |
| `components/web_server/web_server.c` | REST routes, captive routes, assets, and controller commands |
| `web/CMakeLists.txt` | Embeds static browser assets as an ESP-IDF component |
| `web/index.html` | Accessible dashboard structure |
| `web/styles.css` | Responsive local dashboard presentation |
| `web/app.js` | REST commands, SSE updates, reconnection, and provisioning UI |
| `test_apps/unit/` | On-target Unity test application for all pure/component tests |
| `test_apps/unit/pytest_unit.py` | `pytest-embedded` runner for all Unity cases |
| `test_apps/host/` | MinGW/CTest runner for hardware-independent logic before board tests |
| `README.md` | Build, flash, provisioning, configuration, and verification instructions |

---

### Task 1: ESP-IDF Project Foundation and Shared Types

**Files:**
- Create: `CMakeLists.txt`
- Create: `sdkconfig.defaults`
- Create: `partitions.csv`
- Create: `main/CMakeLists.txt`
- Create: `main/app_main.c`
- Create: `components/app_types/CMakeLists.txt`
- Create: `components/app_types/include/app_types.h`
- Create: `test_apps/unit/CMakeLists.txt`
- Create: `test_apps/unit/sdkconfig.defaults`
- Create: `test_apps/unit/main/CMakeLists.txt`
- Create: `test_apps/unit/main/test_main.c`
- Create: `test_apps/unit/main/test_app_types.c`
- Create: `test_apps/unit/pytest_unit.py`

**Interfaces:**
- Consumes: ESP-IDF headers `esp_err.h` and standard C scalar types.
- Produces: `heat_state_t`, `heat_fault_t`, `temperature_sample_t`, and `app_status_t` from `app_types.h` for every later task.

- [ ] **Step 1: Write the shared-type test first**

Create `test_apps/unit/main/test_app_types.c` with exact enum/default expectations:

```c
#include "unity.h"
#include "app_types.h"

TEST_CASE("status starts disabled with relays off", "[app_types]")
{
    app_status_t status = app_status_initial();
    TEST_ASSERT_EQUAL(HEAT_STATE_BOOT, status.state);
    TEST_ASSERT_EQUAL(HEAT_FAULT_NONE, status.fault);
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.heater_on);
    TEST_ASSERT_FALSE(status.fan_on);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, status.target_c);
}
```

- [ ] **Step 2: Create the minimal test application and verify the missing header fails**

Use this runner in `test_apps/unit/main/test_main.c`:

```c
#include "unity.h"

void app_main(void)
{
    unity_run_menu();
}
```

Register `test_main.c` and `test_app_types.c` with `PRIV_REQUIRES unity app_types`. Run:

```powershell
idf.py -C test_apps/unit set-target esp32c3
idf.py -C test_apps/unit build
```

Expected: FAIL because `app_types.h` and the `app_types` component do not exist.

- [ ] **Step 3: Implement shared types and initial status**

Define the exact public model in `components/app_types/include/app_types.h`:

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HEAT_STATE_BOOT = 0,
    HEAT_STATE_PROVISIONING,
    HEAT_STATE_IDLE,
    HEAT_STATE_HEATING,
    HEAT_STATE_HOLDING,
    HEAT_STATE_FAULT,
} heat_state_t;

typedef enum {
    HEAT_FAULT_NONE = 0,
    HEAT_FAULT_SENSOR_OPEN,
    HEAT_FAULT_SENSOR_SHORT,
    HEAT_FAULT_SENSOR_RANGE,
    HEAT_FAULT_SENSOR_INVALID,
    HEAT_FAULT_SENSOR_STALE,
    HEAT_FAULT_OVERTEMP,
    HEAT_FAULT_NVS,
    HEAT_FAULT_OUTPUT_INIT,
} heat_fault_t;

typedef struct {
    float raw_c;
    float filtered_c;
    bool valid;
    uint8_t consecutive_valid;
    uint64_t timestamp_ms;
    heat_fault_t fault;
} temperature_sample_t;

typedef struct {
    heat_state_t state;
    heat_fault_t fault;
    bool enabled;
    bool heater_on;
    bool fan_on;
    float target_c;
    temperature_sample_t temperature;
    int8_t wifi_rssi;
    char ip_addr[16];
    uint64_t uptime_ms;
} app_status_t;

static inline app_status_t app_status_initial(void)
{
    return (app_status_t) {
        .state = HEAT_STATE_BOOT,
        .fault = HEAT_FAULT_NONE,
        .enabled = false,
        .heater_on = false,
        .fan_on = false,
        .target_c = 60.0f,
        .temperature = { .valid = false },
        .wifi_rssi = 0,
        .ip_addr = "0.0.0.0",
        .uptime_ms = 0,
    };
}
```

Create the root project for `project(p1s_heat_controller)` and a minimal `app_main()` that logs startup without initializing hardware. Configure native USB Serial/JTAG console, the custom partition file, no OTA partition, and an ESP main-task stack of 6144 bytes in `sdkconfig.defaults`.

Use this exact pytest runner in `test_apps/unit/pytest_unit.py`:

```python
import pytest


@pytest.mark.esp32c3
def test_all_unity_cases(dut):
    dut.run_all_single_board_cases()
```

- [ ] **Step 4: Build the firmware and unit application**

Run:

```powershell
idf.py set-target esp32c3
idf.py build
idf.py -C test_apps/unit build
```

Expected: both builds finish successfully and the test application contains the `[app_types]` test.

- [ ] **Step 5: Flash and run the first Unity test**

From an ESP-IDF PowerShell environment:

```powershell
$p1sPort = (Get-CimInstance Win32_SerialPort | Where-Object Description -Match 'USB|JTAG|Serial' | Select-Object -First 1 -ExpandProperty DeviceID)
pytest test_apps/unit/pytest_unit.py --target esp32c3 --embedded-services esp,idf --port $p1sPort -s
```

Expected: the `status starts disabled with relays off` case passes.

When no ESP32-C3 is attached, configure and run the equivalent host test with MinGW, then retain the Unity image for the board-verification checkpoint:

```powershell
$p1sCmake = 'C:\Users\dawud\.espressif\tools\cmake\4.0.3\bin\cmake.exe'
& $p1sCmake -S test_apps/host -B test_apps/host/build -G 'MinGW Makefiles'
& $p1sCmake --build test_apps/host/build
& $p1sCmake --build test_apps/host/build --target test
```

Expected: `app_types_initial_status` passes; the on-device Unity run remains required before hardware commissioning.

- [ ] **Step 6: Commit the foundation**

```powershell
git add CMakeLists.txt sdkconfig.defaults partitions.csv main components/app_types test_apps/unit
git commit -m "build: scaffold ESP-IDF heat controller"
```

---

### Task 2: MAX31865 and PT100 Driver

**Files:**
- Create: `components/max31865/CMakeLists.txt`
- Create: `components/max31865/include/max31865.h`
- Create: `components/max31865/max31865.c`
- Create: `test_apps/unit/main/test_max31865.c`
- Modify: `test_apps/unit/main/CMakeLists.txt`

**Interfaces:**
- Consumes: ESP-IDF SPI master API and GPIO4/5/6/7 from its configuration object.
- Produces: `max31865_init(max31865_t *, const max31865_config_t *)`, `max31865_read(max31865_t *, max31865_reading_t *)`, `max31865_resistance_to_celsius(float, float)`, and `max31865_fault_from_register(uint8_t)`.

- [ ] **Step 1: Write conversion and fault-decoding tests**

Create `test_max31865.c`:

```c
#include "unity.h"
#include "max31865.h"

TEST_CASE("PT100 conversion matches IEC points", "[max31865]")
{
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f,
        max31865_resistance_to_celsius(100.0f, 100.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.10f, 60.0f,
        max31865_resistance_to_celsius(123.2419f, 100.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.10f, 80.0f,
        max31865_resistance_to_celsius(130.8968f, 100.0f));
}

TEST_CASE("MAX31865 fault register maps open and short faults", "[max31865]")
{
    TEST_ASSERT_EQUAL(MAX31865_FAULT_OPEN,
        max31865_fault_from_register(0x80));
    TEST_ASSERT_EQUAL(MAX31865_FAULT_SHORT,
        max31865_fault_from_register(0x28));
    TEST_ASSERT_EQUAL(MAX31865_FAULT_NONE,
        max31865_fault_from_register(0x00));
}
```

- [ ] **Step 2: Build to verify the driver test fails**

Run `idf.py -C test_apps/unit build`.

Expected: FAIL because `max31865.h` is missing.

- [ ] **Step 3: Implement the public driver model and pure conversion**

Define:

```c
typedef enum {
    MAX31865_FAULT_NONE = 0,
    MAX31865_FAULT_OPEN,
    MAX31865_FAULT_SHORT,
    MAX31865_FAULT_RANGE,
    MAX31865_FAULT_IO,
} max31865_fault_t;

typedef struct {
    spi_host_device_t host;
    gpio_num_t mosi_io;
    gpio_num_t miso_io;
    gpio_num_t sclk_io;
    gpio_num_t cs_io;
    float rref_ohms;
    float r0_ohms;
} max31865_config_t;

typedef struct {
    spi_device_handle_t spi;
    max31865_config_t config;
} max31865_t;

typedef struct {
    uint16_t adc_code;
    float resistance_ohms;
    float temperature_c;
    uint8_t fault_register;
    max31865_fault_t fault;
} max31865_reading_t;
```

For non-negative temperatures, solve `R/R0 = 1 + A*T + B*T*T` with `A = 3.9083e-3` and `B = -5.775e-7`. For negative values, use the standard fifth-order inverse approximation. Convert ADC code with `resistance = adc_code * rref / 32768.0f`.

- [ ] **Step 4: Implement SPI initialization and reads**

Configure SPI master at 1 MHz, mode 1, and queue size 1. Program the MAX31865 configuration register for bias enabled, automatic conversion, three-wire compensation, and 50 Hz rejection. Wait at least 21 ms after configuration before the first sample. Read RTD MSB/LSB and fault status in one transaction, mask the RTD fault bit, and clear reported faults before returning.

Return `ESP_ERR_INVALID_STATE` when `max31865_read()` is called before initialization and `ESP_ERR_INVALID_RESPONSE` when the fault register is nonzero, while still filling `max31865_reading_t` for diagnostics.

- [ ] **Step 5: Run the MAX31865 tests**

Run the unit test application with the Task 1 pytest command.

Expected: all `[max31865]` and `[app_types]` tests pass.

- [ ] **Step 6: Perform an unloaded sensor smoke test**

Add a temporary test case that initializes SPI with GPIO4/5/6/7, reads the connected PT100, and asserts `10.0f < temperature_c && temperature_c < 45.0f` at room temperature. Run it once on the SuperMini, then remove only that environment-dependent assertion while retaining the production driver.

Expected: a finite ambient reading and a zero MAX31865 fault register.

- [ ] **Step 7: Commit the sensor driver**

```powershell
git add components/max31865 test_apps/unit/main
git commit -m "feat: add MAX31865 PT100 driver"
```

---

### Task 3: Temperature Filtering and Health Monitoring

**Files:**
- Create: `components/temperature/CMakeLists.txt`
- Create: `components/temperature/include/temperature_monitor.h`
- Create: `components/temperature/temperature_monitor.c`
- Create: `test_apps/unit/main/test_temperature_monitor.c`
- Modify: `test_apps/unit/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `max31865_reading_t`, `max31865_fault_t`, monotonic milliseconds, and a calibration offset.
- Produces: `temperature_monitor_init()`, `temperature_monitor_push()`, `temperature_monitor_snapshot()`, and a `temperature_sample_t` with raw, median-filtered, valid-count, timestamp, and mapped `heat_fault_t`.

- [ ] **Step 1: Write failing filter, validity, and staleness tests**

```c
#include "unity.h"
#include "temperature_monitor.h"

TEST_CASE("median filter rejects one temperature spike", "[temperature]")
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 1.5f);
    const float values[] = { 40.0f, 40.2f, 90.0f, 40.1f, 39.9f };
    for (size_t i = 0; i < 5; ++i) {
        temperature_monitor_push(&monitor, values[i], MAX31865_FAULT_NONE, 500 * i);
    }
    temperature_sample_t sample = temperature_monitor_snapshot(&monitor, 2000);
    TEST_ASSERT_TRUE(sample.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 41.6f, sample.filtered_c);
}

TEST_CASE("sample becomes stale after two seconds", "[temperature]")
{
    temperature_monitor_t monitor;
    temperature_monitor_init(&monitor, 0.0f);
    temperature_monitor_push(&monitor, 25.0f, MAX31865_FAULT_NONE, 1000);
    temperature_sample_t sample = temperature_monitor_snapshot(&monitor, 3001);
    TEST_ASSERT_FALSE(sample.valid);
    TEST_ASSERT_EQUAL(HEAT_FAULT_SENSOR_STALE, sample.fault);
}
```

- [ ] **Step 2: Build and confirm failure**

Run `idf.py -C test_apps/unit build`.

Expected: FAIL because the temperature-monitor interface is absent.

- [ ] **Step 3: Implement a five-value median window**

Define `temperature_monitor_t` with a circular array of five floats, sample count, consecutive-valid count, calibration offset, last timestamp, and current fault. Apply calibration before inserting into the window. Until five samples exist, compute the median of the populated values. Mark the sample eligible for enabling only when `consecutive_valid >= 3`.

Map MAX31865 faults exactly:

```c
MAX31865_FAULT_OPEN  -> HEAT_FAULT_SENSOR_OPEN
MAX31865_FAULT_SHORT -> HEAT_FAULT_SENSOR_SHORT
MAX31865_FAULT_RANGE -> HEAT_FAULT_SENSOR_RANGE
MAX31865_FAULT_IO    -> HEAT_FAULT_SENSOR_INVALID
```

Reject non-finite temperature values as `HEAT_FAULT_SENSOR_INVALID`. Use a strict stale comparison of `now_ms - timestamp_ms > 2000`.

- [ ] **Step 4: Add boundary tests for valid-count reset and calibration**

Add tests proving that a sensor fault resets `consecutive_valid` to zero, three following valid readings restore validity, and a `+1.5` calibration offset changes both raw and filtered values by exactly 1.5 degrees.

- [ ] **Step 5: Run all temperature tests**

Run the pytest command from Task 1.

Expected: all `[temperature]`, `[max31865]`, and `[app_types]` tests pass.

- [ ] **Step 6: Commit temperature processing**

```powershell
git add components/temperature test_apps/unit/main
git commit -m "feat: validate and filter chamber temperature"
```

---

### Task 4: Pure Heat-Control State Machine

**Files:**
- Create: `components/controller/CMakeLists.txt`
- Create: `components/controller/include/heat_controller.h`
- Create: `components/controller/heat_controller.c`
- Create: `test_apps/unit/main/test_heat_controller.c`
- Modify: `test_apps/unit/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `temperature_sample_t`, `heat_controller_command_t`, and monotonic milliseconds.
- Produces: `heat_controller_init()`, `heat_controller_command()`, `heat_controller_step()`, and `heat_controller_snapshot()` without direct GPIO, NVS, or network calls.

- [ ] **Step 1: Define the controller API in the failing tests**

Use this public shape in tests:

```c
typedef enum {
    HEAT_COMMAND_ENABLE,
    HEAT_COMMAND_DISABLE,
    HEAT_COMMAND_SET_TARGET,
    HEAT_COMMAND_ACK_FAULT,
    HEAT_COMMAND_SET_PROVISIONING,
    HEAT_COMMAND_LATCH_EXTERNAL_FAULT,
} heat_controller_command_kind_t;

typedef struct {
    heat_controller_command_kind_t kind;
    float target_c;
} heat_controller_command_t;

typedef struct {
    float target_c;
    float hysteresis_c;
    float overtemp_c;
    uint32_t min_transition_ms;
} heat_controller_config_t;
```

Write tests for these exact cases:

```c
TEST_CASE("enable starts heating below target", "[controller]");
TEST_CASE("controller holds at target and reheats one degree below", "[controller]");
TEST_CASE("target zero disables immediately", "[controller]");
TEST_CASE("invalid sensor latches fault and forces output off", "[controller]");
TEST_CASE("raw or filtered 82 degrees latches overtemperature", "[controller]");
TEST_CASE("acknowledgement returns idle but never re-enables", "[controller]");
TEST_CASE("normal relay chatter is blocked for five seconds", "[controller]");
TEST_CASE("fault bypasses the minimum transition interval", "[controller]");
TEST_CASE("external startup fault latches with output off", "[controller]");
```

For the hysteresis case, use a valid sample at 57.0 degrees with target 60, then 60.0, then 59.1, then 59.0. Assert output states `on, off, off, on` with timestamps spaced beyond the five-second minimum.

- [ ] **Step 2: Build and verify tests fail**

Run `idf.py -C test_apps/unit build`.

Expected: FAIL because `heat_controller.h` is missing.

- [ ] **Step 3: Implement deterministic state transitions**

Define `heat_controller_t` as owned mutable state and return an `app_status_t`-compatible snapshot. Initialize it with:

```c
(heat_controller_config_t) {
    .target_c = 60.0f,
    .hysteresis_c = 1.0f,
    .overtemp_c = 82.0f,
    .min_transition_ms = 5000,
}
```

Process immediate safety transitions before the minimum-transition check. `ENABLE` succeeds only when target is above zero, the sample is valid, and `consecutive_valid >= 3`. `ACK_FAULT` succeeds only when sensor faults have cleared for three samples or an over-temperature reading is below 80 degrees. A successful acknowledgement always returns `HEAT_STATE_IDLE` with `enabled = false` and `output_on = false`.

`HEAT_COMMAND_LATCH_EXTERNAL_FAULT` accepts only `HEAT_FAULT_NVS` or `HEAT_FAULT_OUTPUT_INIT`, forces output off, and enters `HEAT_STATE_FAULT`. NVS faults can be acknowledged into IDLE because settings have already fallen back to validated defaults. Output-initialization faults cannot be acknowledged in software.

- [ ] **Step 4: Add target validation as a pure function**

Expose:

```c
bool heat_target_is_valid(float target_c);
```

Return true only for finite values in `[0.0, 80.0]` whose doubled value is within `0.001` of an integer. Add tests for `0`, `0.5`, `60`, and `80` as valid, and `-0.5`, `60.2`, `80.5`, `NAN`, and `INFINITY` as invalid.

- [ ] **Step 5: Run controller tests**

Run the Task 1 pytest command.

Expected: every `[controller]` case passes without modifying any GPIO.

- [ ] **Step 6: Commit the state machine**

```powershell
git add components/controller test_apps/unit/main
git commit -m "feat: add deterministic heat state machine"
```

---

### Task 5: Persistent Settings

**Files:**
- Create: `components/settings/CMakeLists.txt`
- Create: `components/settings/include/settings.h`
- Create: `components/settings/settings.c`
- Create: `test_apps/unit/main/test_settings.c`
- Modify: `test_apps/unit/main/CMakeLists.txt`

**Interfaces:**
- Consumes: NVS namespace `heat_cfg` and `heat_target_is_valid()`.
- Produces: `settings_init()`, `settings_load(settings_data_t *, settings_load_result_t *)`, `settings_set_target()`, `settings_set_calibration()`, and `settings_factory_defaults()`.

- [ ] **Step 1: Write failing default and persistence tests**

```c
#include "nvs_flash.h"
#include "unity.h"
#include "settings.h"

TEST_CASE("empty NVS loads versioned defaults", "[settings]")
{
    TEST_ESP_OK(nvs_flash_erase());
    TEST_ESP_OK(nvs_flash_init());
    settings_data_t data;
    settings_load_result_t result = SETTINGS_LOAD_LOADED;
    TEST_ESP_OK(settings_load(&data, &result));
    TEST_ASSERT_EQUAL(SETTINGS_LOAD_DEFAULT_CREATED, result);
    TEST_ASSERT_EQUAL_UINT32(1, data.schema_version);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, data.target_c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, data.calibration_offset_c);
}

TEST_CASE("target persists but enabled state is not part of settings", "[settings]")
{
    TEST_ESP_OK(settings_set_target(55.5f));
    settings_data_t data;
    settings_load_result_t result = SETTINGS_LOAD_RECOVERED_CORRUPT;
    TEST_ESP_OK(settings_load(&data, &result));
    TEST_ASSERT_EQUAL(SETTINGS_LOAD_LOADED, result);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 55.5f, data.target_c);
    TEST_ASSERT_EQUAL(sizeof(settings_data_t),
        sizeof(data.schema_version) + sizeof(data.target_c) + sizeof(data.calibration_offset_c));
}
```

- [ ] **Step 2: Build and verify failure**

Run `idf.py -C test_apps/unit build`.

Expected: FAIL because `settings.h` is missing.

- [ ] **Step 3: Implement the versioned NVS schema**

Define:

```c
typedef enum {
    SETTINGS_LOAD_LOADED = 0,
    SETTINGS_LOAD_DEFAULT_CREATED,
    SETTINGS_LOAD_RECOVERED_CORRUPT,
} settings_load_result_t;

typedef struct {
    uint32_t schema_version;
    float target_c;
    float calibration_offset_c;
} settings_data_t;
```

Store the structure as a blob under namespace `heat_cfg`, key `settings`, with schema version 1. `settings_load(data, out_result)` returns `SETTINGS_LOAD_DEFAULT_CREATED` for a missing first-run key, `SETTINGS_LOAD_LOADED` for a valid blob, and `SETTINGS_LOAD_RECOVERED_CORRUPT` for a size mismatch, unknown version, invalid target, non-finite calibration, or read error after the namespace exists. Recovery returns defaults and overwrites the invalid blob. Runtime records `HEAT_FAULT_NVS` only for `SETTINGS_LOAD_RECOVERED_CORRUPT`. Limit calibration offset to `[-10.0f, 10.0f]`. Commit NVS after each accepted setter.

- [ ] **Step 4: Add corruption and validation tests**

Write a malformed blob directly through NVS and assert `settings_load()` restores defaults and returns `SETTINGS_LOAD_RECOVERED_CORRUPT`. Assert `settings_set_target(60.2f)` and `settings_set_calibration(11.0f)` return `ESP_ERR_INVALID_ARG` without changing stored values.

- [ ] **Step 5: Run settings tests**

Run the Task 1 pytest command.

Expected: all `[settings]` tests pass after each test erases or resets its namespace.

- [ ] **Step 6: Commit persistence**

```powershell
git add components/settings test_apps/unit/main
git commit -m "feat: persist validated heat settings"
```

---

### Task 6: Relay Outputs and Runtime Ownership

**Files:**
- Create: `components/board/CMakeLists.txt`
- Create: `components/board/Kconfig`
- Create: `components/board/include/board_outputs.h`
- Create: `components/board/board_outputs.c`
- Create: `components/runtime/CMakeLists.txt`
- Create: `components/runtime/include/app_runtime.h`
- Create: `components/runtime/app_runtime.c`
- Create: `test_apps/unit/main/test_board_outputs.c`
- Create: `test_apps/unit/main/test_runtime.c`
- Modify: `test_apps/unit/main/CMakeLists.txt`

**Interfaces:**
- Consumes: MAX31865, temperature monitor, heat controller, settings, GPIO1/GPIO3, and FreeRTOS queues.
- Produces: `board_outputs_init()`, `board_outputs_set(bool)`, `board_outputs_force_off()`, `app_runtime_start()`, `app_runtime_start_with_ops()`, `app_runtime_report_startup_fault()`, `app_runtime_set_target()`, `app_runtime_set_enabled()`, `app_runtime_ack_fault()`, and `app_runtime_get_status()`.

- [ ] **Step 1: Write failing paired-output ordering tests with injected GPIO writes**

Define the driver injection boundary:

```c
typedef void (*board_gpio_write_fn)(int gpio_num, int level, void *ctx);

typedef struct {
    bool active_low;
    board_gpio_write_fn write;
    void *write_ctx;
} board_outputs_config_t;
```

Use a fake writer that records `(gpio, level)` pairs. Assert activation writes fan GPIO1 before heater GPIO3, deactivation writes heater GPIO3 before fan GPIO1, and both logical states are identical after each call. Add an initialization test proving the first two writes request the inactive level.

- [ ] **Step 2: Build and verify failure**

Run `idf.py -C test_apps/unit build`.

Expected: FAIL because board and runtime components are absent.

- [ ] **Step 3: Implement safe paired GPIO output control**

Configure GPIO1 and GPIO3 as outputs with pulls matching `CONFIG_P1S_RELAY_ACTIVE_LOW`. `board_outputs_init()` must set both inactive before returning. Protect paired writes with a mutex. Maintain a cached `bool is_on` so repeated calls do not toggle relays unnecessarily. Map logical state to electrical level with:

```c
static int relay_level(bool on)
{
    return CONFIG_P1S_RELAY_ACTIVE_LOW ? !on : on;
}
```

Add Kconfig entries under `components/board/Kconfig` for active-low selection, defaulting to active-low.

- [ ] **Step 4: Write failing runtime command tests**

Inject fake sensor and output operations into `app_runtime`. Prove that:

- startup publishes target 60 but remains disabled with outputs off;
- a target update is validated, persisted, and reflected in status;
- enable with three valid samples below target enters HEATING and invokes one paired-on call;
- sensor fault invokes paired-off immediately;
- Wi-Fi status changes update RSSI/IP without changing controller state.

- [ ] **Step 5: Implement runtime tasks and queues**

Use these public functions:

```c
typedef struct {
    esp_err_t (*read_sensor)(max31865_reading_t *out_reading, void *ctx);
    esp_err_t (*set_outputs)(bool on, void *ctx);
    esp_err_t (*save_target)(float target_c, void *ctx);
    void *ctx;
} app_runtime_ops_t;

esp_err_t app_runtime_start(void);
esp_err_t app_runtime_start_with_ops(const app_runtime_ops_t *ops);
esp_err_t app_runtime_report_startup_fault(heat_fault_t fault);
esp_err_t app_runtime_set_target(float target_c);
esp_err_t app_runtime_set_enabled(bool enabled);
esp_err_t app_runtime_ack_fault(void);
void app_runtime_set_network_status(int8_t rssi, const char *ip_addr);
void app_runtime_get_status(app_status_t *out_status);
```

Create a length-one overwrite queue for latest temperature samples and a length-eight command queue. The temperature task reads MAX31865 every 500 ms. The controller task wakes on commands or every 100 ms, calls the pure controller, updates outputs only when requested state changes, and copies a complete status snapshot under a mutex. No other component receives a `board_outputs` handle.

`app_runtime_start()` constructs the production operations and delegates to `app_runtime_start_with_ops()`. Tests call only the injected form. If settings loading returns `SETTINGS_LOAD_RECOVERED_CORRUPT`, call `app_runtime_report_startup_fault(HEAT_FAULT_NVS)` after runtime startup; the fault is visible and acknowledged like other latched faults while outputs remain off.

- [ ] **Step 6: Run relay and runtime tests**

Run the Task 1 pytest command.

Expected: `[board]` and `[runtime]` tests pass along with all earlier suites.

- [ ] **Step 7: Verify unloaded relay behavior on hardware**

Leave all COM/NO contacts disconnected. Flash firmware with relay test logging and use the dashboard-independent serial command test to enter HEATING/HOLDING. Verify both onboard relay indicators switch together and remain off through reset.

Expected: both channels energize only in HEATING, and neither channel pulses during reset.

- [ ] **Step 8: Commit runtime ownership**

```powershell
git add components/board components/runtime test_apps/unit/main
git commit -m "feat: own paired relays in controller runtime"
```

---

### Task 7: Wi-Fi Provisioning, Reconnection, Captive DNS, and mDNS

**Files:**
- Create: `components/wifi_manager/CMakeLists.txt`
- Create: `components/wifi_manager/include/wifi_manager.h`
- Create: `components/wifi_manager/wifi_manager.c`
- Create: `components/wifi_manager/captive_dns.c`
- Create: `components/wifi_manager/include/captive_dns.h`
- Create: `test_apps/unit/main/test_wifi_config.c`
- Modify: `test_apps/unit/main/CMakeLists.txt`

**Interfaces:**
- Consumes: ESP Wi-Fi/event/netif/NVS/mDNS APIs and `app_runtime_set_network_status()`.
- Produces: `wifi_manager_start()`, `wifi_manager_scan()`, `wifi_manager_configure()`, `wifi_manager_reset_credentials()`, and `wifi_manager_get_status()`.

- [ ] **Step 1: Write failing credential-validation tests**

Define:

```c
typedef struct {
    bool provisioned;
    bool connected;
    char ssid[33];
    char ip_addr[16];
    int8_t rssi;
} wifi_manager_status_t;

bool wifi_credentials_are_valid(const char *ssid, const char *password);
```

Test that SSID lengths 1 through 32 are accepted, empty and 33-byte SSIDs are rejected, and passwords are either empty or 8 through 63 bytes. Reject null pointers and non-terminated input copied into fixed-size request structures.

- [ ] **Step 2: Build and verify failure**

Run `idf.py -C test_apps/unit build`.

Expected: FAIL because `wifi_manager.h` does not exist.

- [ ] **Step 3: Implement station connection and bounded reconnect**

Initialize NVS, `esp_netif`, the default event loop, Wi-Fi, and default STA/AP interfaces exactly once. Store credentials in namespace `wifi_cfg` under keys `ssid` and `password`. On valid saved credentials, start station mode and reconnect with delays of 1, 2, 4, 8, and then 30 seconds capped thereafter. Publish RSSI and IPv4 text to runtime on connection; publish `0.0.0.0` on disconnect without altering heat state.

- [ ] **Step 4: Implement provisioning SoftAP and captive DNS**

When credentials are missing or rejected, start AP mode at `192.168.4.1` with SSID `P1S-Heat-XXXX`, where `XXXX` is the final two MAC bytes in uppercase hexadecimal. Start a UDP DNS task on port 53 that returns `192.168.4.1` for A-record questions and ignores unsupported record types. Cap each DNS request at 512 bytes and reject malformed label lengths.

Expose scan results through a caller-owned array:

```c
esp_err_t wifi_manager_scan(wifi_ap_record_t *records,
                            uint16_t capacity,
                            uint16_t *out_count);
esp_err_t wifi_manager_configure(const char *ssid, const char *password);
esp_err_t wifi_manager_reset_credentials(void);
```

After successful configuration, stop captive DNS, connect as a station, and advertise `p1s-heat.local` using mDNS service `_http._tcp` on port 80.

- [ ] **Step 5: Add DNS parser and credential persistence tests**

Test a byte-for-byte DNS A response using the query for `connectivitycheck.gstatic.com`, confirm its transaction ID is preserved, and confirm the returned IPv4 bytes are `192,168,4,1`. Test that malformed and oversized queries return zero response length. Erase `wifi_cfg`, save credentials, recreate the manager state, and confirm the exact SSID is loaded.

- [ ] **Step 6: Run Wi-Fi component tests**

Run the Task 1 pytest command.

Expected: credential, DNS, and stored-configuration tests pass.

- [ ] **Step 7: Perform provisioning and outage tests on hardware**

Erase Wi-Fi credentials, flash the main firmware, join `P1S-Heat-XXXX`, and confirm a browser request is redirected to the provisioning page at `192.168.4.1`. Save credentials, confirm station connection and `p1s-heat.local`, then temporarily disable the access point and verify serial logs show reconnect attempts while the controller state remains unchanged.

- [ ] **Step 8: Commit networking**

```powershell
git add components/wifi_manager test_apps/unit/main
git commit -m "feat: add local Wi-Fi provisioning"
```

---

### Task 8: Diagnostics, JSON Contract, REST API, and SSE

**Files:**
- Create: `components/diagnostics/CMakeLists.txt`
- Create: `components/diagnostics/include/diagnostics.h`
- Create: `components/diagnostics/diagnostics.c`
- Create: `components/web_server/CMakeLists.txt`
- Create: `components/web_server/include/api_contract.h`
- Create: `components/web_server/include/web_server.h`
- Create: `components/web_server/api_contract.c`
- Create: `components/web_server/sse_hub.c`
- Create: `components/web_server/include/sse_hub.h`
- Create: `components/web_server/web_server.c`
- Create: `test_apps/unit/main/test_diagnostics.c`
- Create: `test_apps/unit/main/test_api_contract.c`
- Modify: `test_apps/unit/main/CMakeLists.txt`

**Interfaces:**
- Consumes: runtime commands/status, settings, Wi-Fi scan/config/reset, cJSON, and ESP HTTP server.
- Produces: `diagnostics_record()`, `diagnostics_snapshot()`, `api_parse_target()`, `api_status_json()`, `web_server_start(const web_server_config_t *)`, `web_server_stop()`, and a maximum-four-client SSE hub.

- [ ] **Step 1: Write failing ring-buffer and API tests**

For diagnostics, insert 40 numbered records and assert the snapshot returns records 8 through 39 in chronological order. Define each record as:

```c
typedef struct {
    uint64_t timestamp_ms;
    heat_state_t from_state;
    heat_state_t to_state;
    heat_fault_t fault;
} diagnostic_record_t;
```

For API parsing, test:

```c
TEST_ASSERT_EQUAL(ESP_OK,
    api_parse_target("{\"target_c\":60.0}", &target));
TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
    api_parse_target("{\"target_c\":60.2}", &target));
TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
    api_parse_target("{\"target_c\":81}", &target));
TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
    api_parse_target("{\"target_c\":\"hot\"}", &target));
```

Serialize a fixed `app_status_t` and assert the JSON contains numeric `raw_c`, `filtered_c`, and `target_c`, boolean `enabled`, `heater_on`, and `fan_on`, stable string names for state/fault, and escaped IP text.

- [ ] **Step 2: Build and verify failure**

Run `idf.py -C test_apps/unit build`.

Expected: FAIL because diagnostics and API-contract headers are absent.

- [ ] **Step 3: Implement diagnostics and pure JSON handling**

Protect the 32-entry ring buffer with a mutex and never allocate while recording. In `api_contract.c`, cap request bodies at 256 bytes, require a JSON object with exactly a numeric `target_c` field for target updates, use `heat_target_is_valid()`, and serialize with `cJSON_PrintUnformatted()`.

Use stable strings:

```text
boot, provisioning, idle, heating, holding, fault
none, sensor_open, sensor_short, sensor_range, sensor_invalid,
sensor_stale, overtemperature, nvs, output_init
```

- [ ] **Step 4: Implement HTTP routes**

Use an injected service boundary so handler tests never start the real controller:

```c
typedef struct {
    void (*get_status)(app_status_t *out_status);
    esp_err_t (*set_target)(float target_c);
    esp_err_t (*set_enabled)(bool enabled);
    esp_err_t (*ack_fault)(void);
    esp_err_t (*wifi_scan)(wifi_ap_record_t *records,
                           uint16_t capacity,
                           uint16_t *out_count);
    esp_err_t (*wifi_configure)(const char *ssid, const char *password);
    esp_err_t (*wifi_reset)(void);
} web_server_ops_t;

typedef struct {
    web_server_ops_t ops;
    uint16_t port;
} web_server_config_t;
```

Register these handlers with `esp_http_server`:

```text
GET  /api/status
PUT  /api/target
POST /api/control/enable
POST /api/control/disable
POST /api/fault/acknowledge
GET  /api/wifi/scan
POST /api/wifi/configure
POST /api/wifi/reset
GET  /api/events
GET  /
GET  /styles.css
GET  /app.js
```

Return JSON errors with HTTP 400 for validation failures, 409 when enable/acknowledge preconditions fail, 503 when runtime queues are unavailable, and 500 for internal errors. Return 204 for successful commands without bodies.

- [ ] **Step 5: Implement bounded Server-Sent Events**

Use `httpd_req_async_handler_begin()` for `/api/events` and retain at most four asynchronous requests. Send headers for `text/event-stream`, `Cache-Control: no-cache`, and `Connection: keep-alive`. A valid frame has the exact form `event: status\ndata: {"state":"idle","target_c":60.0}\n\n`; broadcast the current complete status JSON once per second and immediately after controller state changes. Remove and complete clients after any send error. Send `: keepalive\n\n` every 15 seconds if status has not changed.

- [ ] **Step 6: Add HTTP handler tests with fake runtime calls**

Start the HTTP server on the device with a fake runtime adapter. Verify valid target updates call `app_runtime_set_target(60.0f)` once, invalid bodies never call it, enable conflicts return 409, and a connected SSE client receives a complete `status` event terminated by a blank line.

- [ ] **Step 7: Run diagnostics and API tests**

Run the Task 1 pytest command.

Expected: `[diagnostics]`, `[api]`, and all preceding suites pass.

- [ ] **Step 8: Commit the service boundary**

```powershell
git add components/diagnostics components/web_server test_apps/unit/main
git commit -m "feat: expose heat controller REST and SSE API"
```

---

### Task 9: Embedded Responsive Web Interface

**Files:**
- Create: `web/CMakeLists.txt`
- Create: `web/index.html`
- Create: `web/styles.css`
- Create: `web/app.js`
- Create: `test_apps/unit/main/test_web_assets.c`
- Modify: `test_apps/unit/CMakeLists.txt`
- Modify: `components/web_server/CMakeLists.txt`

**Interfaces:**
- Consumes: the Task 8 REST/SSE contract.
- Produces: embedded symbols `_binary_index_html_start/end`, `_binary_styles_css_start/end`, and `_binary_app_js_start/end`, served by the web server.

- [ ] **Step 1: Write a failing embedded-asset smoke test**

```c
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

TEST_CASE("dashboard asset contains required controls", "[web]")
{
    size_t length = index_html_end - index_html_start;
    TEST_ASSERT_GREATER_THAN(200, length);
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"current-temperature\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"target-temperature\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"enable-button\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"fault-panel\""));
}
```

- [ ] **Step 2: Build and verify failure**

Run `idf.py -C test_apps/unit build`.

Expected: FAIL because the embedded symbols and assets do not exist.

- [ ] **Step 3: Implement the dashboard structure and styling**

Create semantic HTML with one `<main>`, visible current temperature, target range and number inputs synchronized at 0.5-degree steps, Enable and Disable controls, heater/fan/state badges, a fault panel with acknowledge button, and a network/settings panel. Add an `aria-live="polite"` status region and keyboard-visible focus states.

Use CSS without frameworks. Support widths down to 320 px, respect `prefers-reduced-motion`, ensure status is not represented by color alone, and show a persistent warning whenever the selected target exceeds 60 degrees Celsius.

- [ ] **Step 4: Implement REST and SSE browser behavior**

In `app.js`:

- load `/api/status` on startup;
- connect `EventSource('/api/events')` and render every `status` event;
- retry by fetching status after SSE errors and let `EventSource` reconnect;
- send target changes through `PUT /api/target` with JSON;
- send Enable, Disable, and Acknowledge through their POST endpoints;
- disable controls while a request is active;
- restore controls and show the server error message after failure;
- set target 0 to disabled presentation;
- never infer heater/fan state from temperature; render the server booleans.

Use `textContent`, never `innerHTML`, for all server-provided values.

- [ ] **Step 5: Embed assets and serve correct content types**

Register `web/` as an ESP-IDF component and embed the three files with `EMBED_TXTFILES`. Add `web` as a private requirement of `web_server`. Serve HTML as `text/html; charset=utf-8`, CSS as `text/css; charset=utf-8`, and JavaScript as `text/javascript; charset=utf-8` with `Cache-Control: no-cache` during development.

- [ ] **Step 6: Build and run web-asset tests**

Run the Task 1 pytest command.

Expected: `[web]` passes and the main firmware links all three embedded symbols.

- [ ] **Step 7: Test the interface in phone and desktop layouts**

Flash the device, open `http://p1s-heat.local`, and verify at approximately 375 px and 1280 px browser widths: live values update, target input and slider stay synchronized, warning appears above 60, fault acknowledgement is disabled when no fault exists, and network loss shows a disconnected state without clearing the last measurement.

- [ ] **Step 8: Commit the local dashboard**

```powershell
git add web components/web_server test_apps/unit/main test_apps/unit/CMakeLists.txt
git commit -m "feat: add embedded heat-control dashboard"
```

---

### Task 10: Application Composition, Full Verification, and Operator Documentation

**Files:**
- Modify: `main/app_main.c`
- Modify: `main/CMakeLists.txt`
- Create: `README.md`
- Create: `docs/commissioning-checklist.md`
- Modify: `test_apps/unit/pytest_unit.py`

**Interfaces:**
- Consumes: all components from Tasks 1 through 9.
- Produces: a bootable, locally provisioned chamber controller and reproducible build/commissioning documentation.

- [ ] **Step 1: Write the startup-order integration test**

Add injectable startup functions around `app_main` and record their order. Assert this exact sequence:

```text
board_outputs_init
nvs_flash_init
settings_init
app_runtime_start
wifi_manager_start
web_server_start
```

Assert that failure before or during any later step calls `board_outputs_force_off()` and never enables runtime control.

- [ ] **Step 2: Implement top-level startup and failure policy**

Initialize relay outputs before NVS, SPI, tasks, networking, or HTTP. Recover from `ESP_ERR_NVS_NO_FREE_PAGES` and `ESP_ERR_NVS_NEW_VERSION_FOUND` by erasing NVS once and reinitializing. If output initialization fails, log `HEAT_FAULT_OUTPUT_INIT` and stop startup. If networking fails, keep the runtime alive and retry networking. If web-server startup fails, keep regulation disabled until the server is available or the device is restarted.

- [ ] **Step 3: Add complete build and unit-test commands to README**

Document these PowerShell commands exactly:

```powershell
idf.py set-target esp32c3
idf.py build
$p1sPort = (Get-CimInstance Win32_SerialPort | Where-Object Description -Match 'USB|JTAG|Serial' | Select-Object -First 1 -ExpandProperty DeviceID)
idf.py -p $p1sPort flash monitor
pytest test_apps/unit/pytest_unit.py --target esp32c3 --embedded-services esp,idf --port $p1sPort -s
```

Document the pin table, PT100/MAX31865 values, relay active-level menuconfig option, provisioning SSID, `p1s-heat.local`, REST endpoints, default target, hysteresis, over-temperature threshold, and restart-disabled behavior.

- [ ] **Step 4: Create a staged commissioning checklist**

The checklist must require recorded pass/fail results for:

1. both relay contacts unloaded during first power-up;
2. both relay indicators off throughout boot and reset;
3. ambient PT100 comparison against a reference thermometer;
4. MAX31865 open and short fault tests;
5. 5 V fan-only relay test;
6. heater integration after the preceding checks pass;
7. targets of 30, 45, and 60 degrees with observed peak overshoot;
8. Wi-Fi disconnect during HEATING and HOLDING;
9. power interruption while enabled, followed by confirmation that restart remains disabled;
10. target 80 test only after lower-temperature tests record stable results.

- [ ] **Step 5: Run clean builds and the complete automated suite**

Run:

```powershell
idf.py fullclean
idf.py build
idf.py -C test_apps/unit fullclean
idf.py -C test_apps/unit build
$p1sPort = (Get-CimInstance Win32_SerialPort | Where-Object Description -Match 'USB|JTAG|Serial' | Select-Object -First 1 -ExpandProperty DeviceID)
pytest test_apps/unit/pytest_unit.py --target esp32c3 --embedded-services esp,idf --port $p1sPort -s
```

Expected: clean firmware and test-app builds succeed; every Unity case passes.

- [ ] **Step 6: Run the full acceptance sequence**

Complete `docs/commissioning-checklist.md` and verify each acceptance criterion from the specification. Capture the firmware version, ESP-IDF version, relay polarity configuration, PT100 calibration offset, fan label current, 5 V supply rating, and measured overshoot values in the checklist.

- [ ] **Step 7: Inspect final logs and repository state**

Run:

```powershell
idf.py size
git status --short
git diff --check
```

Expected: firmware fits the factory partition, no whitespace errors exist, and only the intended implementation/documentation files are uncommitted.

- [ ] **Step 8: Commit the integrated MVP**

```powershell
git add main README.md docs/commissioning-checklist.md
git commit -m "feat: integrate P1S chamber heat controller"
```

- [ ] **Step 9: Tag the verified breadboard release only after commissioning passes**

```powershell
git tag -a v0.1.0 -m "P1S heat controller breadboard MVP"
git status --short
```

Expected: working tree is clean and tag `v0.1.0` points to the verified integration commit.
