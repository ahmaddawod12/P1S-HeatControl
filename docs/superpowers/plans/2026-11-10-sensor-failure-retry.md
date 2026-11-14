# Sensor Failure Retry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Tolerate four consecutive failed sensor reads, latch on the fifth,
and sample once per second with a 10-second fan cooldown after fault shutdown.

**Architecture:** Count failed acquisitions in the temperature monitor and
carry that count with each sample. The controller holds its existing output
during retries; acquisition staleness remains independently detectable. The
runtime output policy owns the fan cooldown after an enabled session faults.

**Tech Stack:** C, ESP-IDF/FreeRTOS, CMake/CTest host tests, Unity target tests.

**Spec:** `docs/superpowers/specs/2026-11-10-sensor-failure-retry-design.md`

## Global Constraints

- Sample every 1000 ms including conversion; controller interval is 500 ms.
- Five consecutive failed acquisitions latch; one success resets the count.
- Failures 1 through 4 retain heater output; they cannot enable heating.
- Over-temperature >= 82 C, Disable, maintenance, and inhibition remain immediate.
- Stalled acquisition faults after 2000 ms even following an invalid reading.
- Require three valid reads for recovery/enable and retain 5000 ms restart delay.
- Fault shutdown from an enabled session runs the fan for 10000 ms.
- Preserve unrelated edits; leave changes uncommitted and do not flash hardware.

## Review Focus

- Reusing one failed sample on 500 ms controller ticks must not count as more reads.
- Alternating sensor errors cannot evade the fifth-failure threshold.
- A stall after an invalid read must still shut down independently of acquisition.
- A first good read that is over-temperature must bypass retry/recovery gates.
- Acknowledgement and repeated fault ticks must not shorten or extend fan cooldown.

### Task 1: Acquisition failures and controller retries

**Files:** `components/app_types/include/app_types.h`,
`components/temperature/include/temperature_monitor.h`,
`components/temperature/temperature_monitor.c`,
`components/controller/heat_controller.c`,
`test_apps/host/test_temperature_monitor.c`,
`test_apps/host/test_heat_controller.c`.

**Interfaces:** Add `uint8_t consecutive_failures` to monitor/sample structures
and a shared failure limit of 5. Existing push/snapshot/step signatures remain.

- [x] Add behavioral tests for four tolerated failures, failure five, reset on
  success, mixed errors, startup invalid data, repeated control ticks, immediate
  over-temperature, and recovery with three valid reads plus restart delay.
- [x] Run the targeted tests and observe failures before changing production code.
- [x] Implement the acquisition counter and controller retry/recovery branches.
- [x] Run `ctest --test-dir test_apps/host/build-sensor-retry-20261110 -R '^(temperature_monitor|heat_controller)$' --output-on-failure`; expect all pass.

### Task 2: Fault fan cooldown

**Files:** `components/runtime/runtime_output_policy.c`,
`test_apps/host/test_runtime_output_policy.c`.

**Interfaces:** Keep the existing output-policy interface and track the
enabled-to-fault transition using existing policy state.

- [x] Replace the old immediate-fan-off fault expectation with tests for a
  10000 ms cooldown from heating and holding, stable deadline across repeated
  fault ticks/acknowledgement, and no fan start for an inactive startup fault.
- [x] Run `runtime_output_policy` and observe the expected failure.
- [x] Implement cooldown on fault shutdown while preserving manual/maintenance behavior.
- [x] Run `ctest --test-dir test_apps/host/build-sensor-retry-20261110 -R '^runtime_output_policy$' --output-on-failure`; expect pass.

### Task 3: Runtime timing, stall handling, and integration

**Files:** `components/runtime/app_runtime.c`,
`test_apps/host/test_runtime_maintenance.c`, `test_apps/host/CMakeLists.txt`,
`test_apps/unit/main/test_runtime.c`, related existing target temperature tests,
`README.md`, `docs/commissioning-checklist.md`.

**Interfaces:** Runtime publishes real failed-read counts; sample timestamps
remain acquisition timestamps so the controller detects a stalled task.

- [x] Add threaded tests for the one-second cadence (including conversion),
  retry/reset/latch behavior, and a stall following a failed read. Update
  existing failure waits to accommodate five real acquisitions and recovery
  waits to accommodate three one-second readings.
- [x] Run new runtime cases and observe expected failures before implementation.
- [x] Change the sample period to 1000 ms and controller period to 500 ms; enforce acquisition age on invalid
  as well as valid snapshots; keep stale faults outside the retry allowance.
- [x] Update current documentation and target-test expectations for the behavior.
- [x] Build host targets and run the full CTest suite, web tests, and tool tests;
  compile production firmware with the installed ESP-IDF toolchain.
- [x] Review the scoped changes with a fresh reviewer; address material findings
  with regression tests, then rerun affected checks and report exact results.
