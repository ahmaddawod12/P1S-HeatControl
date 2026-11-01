# DS18B20 Sensor Migration Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Replace the production NTC acquisition with a DS18B20 on GPIO27.

**Architecture:** Add a sensor adapter around Espressif's DS18B20/RMT drivers.
Pass sensor-neutral readings through the existing monitor and controller.
Migrate calibration separately from target and Wi-Fi settings.

**Tech Stack:** C, ESP-IDF 6.1, espressif/ds18b20 0.4.0, espressif/onewire_bus 1.1.2, CMake/CTest, Unity.

**Spec:** `docs/superpowers/specs/2026-11-01-ds18b20-sensor-design.md`

## Global Constraints

- Classic `esp32`, GPIO27 DATA, 3.3 V external power, external DATA pull-up.
- 11-bit samples every 500 ms; independent 100 ms control and 2 s stale timeout.
- Preserve relay pins, API fault names, three-valid-sample gate and 82 °C trip.
- Preserve existing changes to maintained code; remove superseded NTC files
  during commit cleanup and leave changes uncommitted for review.
- No flashing or publishing; the probe cable is limited to 85 °C.

## Review Focus

- A probe absent at boot must leave local service available and heating off.
- Reconnecting a powered-down probe must reapply conversion resolution.
- CRC failures, stuck-low DATA and nonfinite readings must never look valid.
- Sensor conversion time must not delay stop/maintenance or freshness checks.
- A previous NTC calibration must not shift the new digital sensor's reading.

### Task 1: DS18B20 acquisition and runtime integration

**Files:** New `components/temperature_sensor/`, sensor types in
`components/app_types/include/temperature_sensor_types.h`, board pin mapping,
temperature/runtime components, host driver tests and stubs, unit tests,
sensor-smoke application and component manifests.

**Interfaces:** `temperature_sensor_config_t`, `temperature_sensor_t`,
`temperature_sensor_reading_t`, `temperature_sensor_fault_t`;
`temperature_sensor_default_config(gpio_num_t)`,
`temperature_sensor_init(temperature_sensor_t *, const temperature_sensor_config_t *)`,
`temperature_sensor_read(temperature_sensor_t *, temperature_sensor_reading_t *)`,
`temperature_sensor_deinit(temperature_sensor_t *)` all returning `esp_err_t`
except default configuration.

- [x] Write and run failing host tests for fresh-conversion sequencing,
  absent/shorted DATA, CRC/transport faults, −127/nonfinite/out-of-range values,
  hot-plug resolution reset and resource cleanup. Expected: new adapter absent.
- [x] Implement the adapter; run the tests and expect PASS.
- [x] Replace production thermistor types/dependencies, preserve reported
  sensor faults on read errors, and subtract conversion time from the 500 ms
  task delay. Adapt existing runtime/monitor tests and smoke output.
- [x] Run the full host suite and build production, Unity and smoke images.

### Task 2: Sensor calibration migration and commissioning guidance

**Files:** `components/settings/`, host/settings tests and NVS stubs,
`test_apps/unit/main/test_settings.c`, `README.md`,
`docs/commissioning-checklist.md`.

**Interfaces:** Existing settings API, schema version 2; schema-1 migration
preserves `target_c`, resets `calibration_offset_c` to 0 and stores schema 2.

- [x] Write a host migration test using a schema-1 blob with target 55.5 °C
  and offset −7 °C. Expected before implementation: offset remains −7 °C.
- [x] Migrate valid legacy records; test persistence, write errors and corrupt
  records. Preserve schema-2 offsets on subsequent loads.
- [x] Update wiring, pull-up/old-divider instructions, cable limits, fault
  injection and smoke-test instructions. Mark old commissioning results as
  historical and requiring repeat for the new sensor.
- [x] Run CTest, dashboard/tool tests and all three firmware builds; review
  only this task's unstaged changes and new files before reporting completion.


## Verification completed 2026-11-01

- Host CTest: 22/22 passed after commit cleanup removed the obsolete NTC test.
- Dashboard tests: 18/18 passed.
- Firmware packaging Python tests: 10/10 passed in the ESP-IDF environment.
- Publisher script: all four repository scenarios passed.
- ESP-IDF 6.1 production, Unity-test and sensor-smoke builds: all exit 0.
- Production component list includes `temperature_sensor`, excludes `thermistor`.
- Independent source review: two Important findings fixed. New tests first
  reproduced missing timeout, stale recovery/filter state and a mid-read short,
  then passed after the fixes; the full host suite stayed green.
- `git diff --check`: passed. Existing staged work preserved; changes uncommitted.
- No flash, signing, publishing or physical probe/relay tests performed.
