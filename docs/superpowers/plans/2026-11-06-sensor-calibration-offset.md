# Sensor Calibration Offset Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan.

**Goal:** Expose the existing sensor offset, default 0 °C, through the dashboard.

**Architecture:** Persist through existing settings and runtime admission,
rebase the monitor when an admitted calibration command runs, and include the
active offset in status/SSE. Reuse the chamber command trust and body limits.

**Tech Stack:** ESP-IDF C, FreeRTOS, NVS, vanilla HTML/CSS/JavaScript, host C tests,
Node's test runner.

**Spec:** `docs/superpowers/specs/2026-11-06-sensor-calibration-design.md`

## Global Constraints

- Default 0 °C; accepted range −10 to +10 °C; preserve saved values.
- Apply after successful persistence without blocking regulation during writes.
- Keep the 512-byte status contract and existing body/Origin/CSRF protections.
- Preserve pre-existing edits in unit sdkconfig defaults and bootstrap tooling.

## Review Focus

- Failed persistence must leave the active correction unchanged.
- Filter history and future samples must receive the same correction exactly once.
- Calibration must preserve sensor faults, freshness, and thermal shutdown.
- Target and calibration saves must serialize without losing either setting.
- Status updates must preserve an offset being edited, including an empty draft.

### Task 1: Live sensor calibration

**Files:** runtime, temperature monitor, app status, host sensor/runtime tests.
**Interfaces:** `temperature_monitor_set_calibration`,
`app_runtime_set_calibration(float)`, `app_status_t.calibration_offset_c`.

- [x] Add and run failing tests for rebase, live signed offsets, failure and restart.
- [x] Add serialized calibration persistence and command application.
- [x] Run monitor, settings, runtime and controller tests.

### Task 2: Dashboard and API

**Files:** API contract, web server, dashboard, host HTTP and dashboard tests, README.
**Interfaces:** protected `PUT /api/calibration`, body `calibration_offset_c`;
status/SSE numeric `calibration_offset_c`.

- [x] Add and run failing request and dashboard behavior tests.
- [x] Add protected route, status serialization, labelled numeric input and Save.
- [x] Document offset direction, range, default and persistence.
- [x] Run full host/dashboard suites, firmware build and review.

## Execution notes

Work in the current checkout on `codex/sensor-calibration-offset`. Keep changes
uncommitted for review. The user requested implementation; proceed directly with
the existing 0 °C default and ±10 °C calibration model.

## Verification results

- Full host build and CTest suite: 84/84 pass.
- `node --test test_apps/web/*.cjs`: 41/41 pass.
- ESP-IDF 6.1 release firmware build: pass.
- ESP-IDF on-device Unity test image build: pass; not flashed or run on hardware.
- `python tools/check_printer_stack.py --build build-release`: pass (3456/4096 bytes).
- `git diff --check`: pass.
- Independent read-only review: no actionable findings. Physical accuracy and
  timing require the device; the two pre-existing user edits were outside review.
