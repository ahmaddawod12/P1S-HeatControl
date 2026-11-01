# ESP-IDF 6.1 Pin Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the ESP32-C3 heat controller on ESP-IDF 6.1 with its thermistor on GPIO0, heater relay on GPIO6, and fan relay on GPIO7.

**Architecture:** Keep all board wiring constants in the board component and make runtime and smoke-test consumers use that single hardware contract. Preserve the thermistor ADC one-shot/curve-fitting path and paired relay sequencing, then verify both the host-independent logic and complete ESP-IDF applications under the installed v6.1 toolchain.

**Tech Stack:** ESP-IDF 6.1, ESP32-C3, C, CMake, Unity, pytest-embedded, MinGW/CTest

**Spec:** `docs/superpowers/specs/2026-08-25-p1s-heat-controller-design.md`, amended by the user's 2026-09-10 GPIO and ESP-IDF 6.1 migration request.

## Global Constraints

- Target remains `esp32c3`.
- NTC divider midpoint is GPIO0 / ADC1 channel 0.
- Heater relay is GPIO6; fan relay is GPIO7.
- Relay active level, fan-before-heater activation, and heater-before-fan deactivation behavior do not change.
- ADC remains calibrated one-shot mode at 12 dB attenuation with a 16-sample average.
- ESP-IDF 6.1 is the supported and verified build version.
- Do not flash or energize mains hardware as part of automated verification.

---

### Task 1: Centralize and migrate the board pin contract

**Files:**
- Create: `components/board/include/board_pins.h`
- Modify: `components/board/include/board_outputs.h`
- Modify: `components/runtime/app_runtime.c`
- Modify: `test_apps/sensor_smoke/main/sensor_smoke.c`
- Modify: `test_apps/unit/main/test_board_outputs.c`
- Modify: `test_apps/unit/main/test_thermistor.c`

**Interfaces:**
- Consumes: ESP-IDF `gpio_num_t` / `GPIO_NUM_*` definitions and the existing `thermistor_default_config(gpio_num_t)` API.
- Produces: `BOARD_THERMISTOR_GPIO`, `BOARD_HEATER_GPIO`, and `BOARD_FAN_GPIO` as the single board wiring contract used by production and test applications.

- [x] **Step 1: Make the hardware-facing tests require the new wiring**

  Change relay-write expectations to literal GPIO7/GPIO6 values so a stale board mapping fails. Make the thermistor default-wiring test build the production board pin into `thermistor_default_config()` and expect GPIO0.

- [x] **Step 2: Build the unit-test application to verify RED**

  Run: `python C:\esp\v6.1\esp-idf\tools\idf.py -C test_apps/unit build`

  Expected: compilation fails because `board_pins.h` / `BOARD_THERMISTOR_GPIO` does not exist, proving the test requires the new production contract.

- [x] **Step 3: Implement the centralized GPIO contract**

  Add `board_pins.h` with:

  ```c
  #pragma once

  #include "driver/gpio.h"

  #define BOARD_THERMISTOR_GPIO GPIO_NUM_0
  #define BOARD_HEATER_GPIO GPIO_NUM_6
  #define BOARD_FAN_GPIO GPIO_NUM_7
  ```

  Include it from `board_outputs.h`; remove the stale GPIO1/GPIO3 definitions. Replace runtime and sensor-smoke private thermistor GPIO constants with `BOARD_THERMISTOR_GPIO`.

- [x] **Step 4: Rebuild the unit-test application to verify GREEN**

  Run: `python C:\esp\v6.1\esp-idf\tools\idf.py -C test_apps/unit build`

  Expected: build exits 0 with tests linked into the ESP32-C3 unit-test image.

- [x] **Step 5: Inspect the task diff**

  Run: `git diff --check` and `git diff -- components/board components/runtime test_apps/unit test_apps/sensor_smoke`.

  Expected: no whitespace errors; only the intended pin-contract/test changes appear. Leave the change uncommitted for user review.

### Task 2: Align documentation and verify ESP-IDF 6.1 applications

**Files:**
- Modify: `README.md`
- Modify: `docs/commissioning-checklist.md`
- Modify: `docs/superpowers/specs/2026-08-25-p1s-heat-controller-design.md`

**Interfaces:**
- Consumes: the board pin contract from Task 1 and Espressif's v6.1 ESP32-C3 GPIO, ADC one-shot, ADC calibration, and migration documentation.
- Produces: build and commissioning instructions matching GPIO0/GPIO6/GPIO7 and ESP-IDF 6.1.

- [x] **Step 1: Update the wiring and SDK documentation**

  Replace the old sensor and GPIO3/GPIO1 relay assignments with GPIO0/GPIO6/GPIO7. State that builds are verified on ESP-IDF 6.1 and retain the existing clean-build/test commands.

- [x] **Step 2: Build the production image under ESP-IDF 6.1**

  Run after activating `C:\esp\v6.1\esp-idf\export.ps1`: `python C:\esp\v6.1\esp-idf\tools\idf.py build`.

  Expected: exit 0 and `build/p1s_heat_controller.bin` generated for ESP32-C3.

- [x] **Step 3: Build the sensor smoke application under ESP-IDF 6.1**

  Run: `python C:\esp\v6.1\esp-idf\tools\idf.py -C test_apps/sensor_smoke build`.

  Expected: exit 0 and a sensor-smoke ESP32-C3 image linked with the GPIO0 thermistor contract.

- [x] **Step 4: Run all host tests**

  Configure with the installed MinGW CMake, build, then run CTest:

  ```powershell
  $p1sCmake = 'C:\Users\dawud\.espressif\tools\cmake\4.0.3\bin\cmake.exe'
  & $p1sCmake -S test_apps/host -B test_apps/host/build -G 'MinGW Makefiles'
  & $p1sCmake --build test_apps/host/build
  & $p1sCmake --build test_apps/host/build --target test
  ```

  Expected: all four host test executables pass.

- [x] **Step 5: Final static and diff verification**

  Run `rg` for stale GPIO1/GPIO3 hardware references and confirm active thermistor references use GPIO0, then run `git diff --check` and `git status --short`.

  Expected: no active code or current documentation uses the old wiring; no whitespace errors; only scoped migration files and this plan are modified.

  On-target Unity execution and unloaded-contact commissioning remain manual because they require the connected ESP32-C3 and safe physical access.
