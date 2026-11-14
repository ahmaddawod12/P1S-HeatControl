# Firmware Review Issue Fixes Implementation Plan

> **For agentic workers:** Use test-driven development and an independent final code review. Preserve the existing uncommitted changes; do not commit, flash, sign, or publish.

**Goal:** Fix the four issues identified by the whole-firmware review and authorized by the user's “Fix these issues.”

**Architecture:** Keep the current ESP-IDF components and interfaces. Fence automatic monitor shutdown against the runtime Disable epoch. Serialize OTA and monitor-start admission with the existing service-operation gate, and reject OTA while monitoring is active to retain the printer TLS heap release needed by updates. Make printer Forget use the same asynchronous acceptance/status pattern as Save. Refresh RSSI without resetting Wi-Fi connection timers.

**Tech Stack:** ESP-IDF 6.1, C11, FreeRTOS, MinGW host tests, browser JavaScript/Node tests.

**Spec:** The four findings in this chat; the existing README and Monitor P1S / OTA contracts remain applicable except DELETE printer configuration now returns 202 and completion is reported in printer status.

## Global Constraints

- Work in the current checkout because the reviewed state includes uncommitted changes.
- Manual Enable, Disable, target and calibration behavior stays available during monitoring.
- A rejected or unsuccessful OTA preflight must preserve monitored heating.
- No device flashing, signing, publication, or Git commits are requested.

## Review Focus

- An old monitor end must not disable heating after a completed or pending owner Disable/Enable sequence.
- Monitor start and OTA admission must not race past each other's checks.
- Forget acceptance must not claim storage completion; status resolves success or failure.
- New edits made during Forget must survive completion.
- RSSI refresh must preserve AP shutdown deadlines and ignore transient radio query failures.

### Task 1: Fence runtime monitor shutdown

**Files:** `components/runtime/app_runtime.c`, `test_apps/host/test_runtime_maintenance.c`, host CMake test registration.

- [x] Add failing host cases for monitor-end after Disable/Enable and after a pending Disable (before the controller consumes it).
- [x] Record the Disable epoch at monitor begin. Under the admission mutex, claim monitor shutdown only if active and its epoch still matches; atomically advance the epoch before requesting automatic Disable.
- [x] Run the new cases and existing runtime tests; confirm both stale cases preserve later manual heating.

### Task 2: Preserve monitoring during rejected OTA requests

**Files:** service-operation header/source, `printer_control.c`, `firmware_update.c`, host coexistence/control/service tests, README and OTA guide.

- [x] Add failing tests proving active-monitor OTA requests are rejected without suspension and monitor starts are blocked during OTA admission.
- [x] Add a transient `SERVICE_OPERATION_PRINTER_MONITOR` owner around coordinator monitor-start admission; release it on every return path. It is never held for the monitoring session.
- [x] Once OTA owns its gate, reject if runtime monitoring is active before busy state or notifications change.
- [x] Verify mutual exclusion, normal update operation, and gate release after rejected starts.

### Task 3: Complete Forget asynchronously

**Files:** `printer_control.c`, printer HTTP handler, `web/printer.js`, coordinator/API/browser tests, README.

- [x] Add failing tests for accepted-but-not-yet-cleared Forget and browser preservation of drafts until storage success, including failure and edits during the operation.
- [x] Remove synchronous completion semaphore/wait. Return queue admission immediately; DELETE returns 202 and existing applying/config_result reports completion.
- [x] Track the pending Forget draft in the dashboard and clear it only on confirmed successful deletion when unchanged. Resolve uncertainty from definitive status without automatic retries.
- [x] Run coordinator, HTTP and dashboard tests.

### Task 4: Refresh RSSI

**Files:** `wifi_manager.c`, native Wi-Fi host fixture, host CMake registration.

- [x] Add failing coverage for changed signal after association, bounded query frequency, query failure, and unchanged AP deadline.
- [x] Refresh RSSI every 5 seconds while connected using the worker's existing publication path; leave connection/AP state intact.
- [x] Run native Wi-Fi and startup/recovery tests.

### Final verification

- [x] Run the complete host and dashboard suites, Python tooling and PowerShell publisher checks.
- [x] Build generic ESP32 firmware and run the printer stack checker.
- [x] Obtain a fresh read-only review of these changes, resolve actionable findings, and report outcomes and hardware-test limits.

## Completion evidence

- Runtime shutdown regressions both failed before the fix and passed afterward; all 28 runtime cases passed.
- OTA, monitor admission and service owner regressions failed before the fix and passed afterward.
- Async Forget coordinator/API and dashboard regressions failed before their fixes and passed afterward.
- Six native RSSI regression scenarios failed before the fix and passed afterward.
- Independent read-only final review found one additional pre-admission dashboard polling race. Three reproductions failed, then passed after guarding status resolution with both request-start and current mutation state. No other scoped findings were reported.
- Final validation: 121/121 host tests, 48/48 dashboard tests, 31/31 Python tooling tests and 10/10 publisher safeguard scenarios passed. Generic ESP32 release build and printer stack analysis passed; git diff --check is clean.
- Existing uncommitted work preserved. No commits, flashing, signing, publication or physical hardware tests performed.