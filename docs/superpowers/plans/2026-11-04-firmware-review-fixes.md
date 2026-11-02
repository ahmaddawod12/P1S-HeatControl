# Firmware review fixes implementation plan

> **For agentic workers:** Use test-driven development and focused independent component work; retain the current checkout and all pre-existing printer edits. Do not commit, flash, sign, or publish.

**Goal:** Fix the six issues accepted in the firmware review.

**Architecture:** Keep runtime admission/output locks short and reserve a target update while persistence runs outside that lock. Reuse the existing HTTP Origin/CSRF policy, reject recursive target JSON before cJSON, abort stalled body reads, and compare SSE status without modifying the emitted snapshot. Preserve completed Wi-Fi initialization stages or unwind partial resources so retries are safe.

**Tech stack:** C, ESP-IDF 6.1/classic ESP32, FreeRTOS, cJSON, browser JavaScript, MinGW host tests.

**Spec:** The six review findings and the user's instruction to fix them in this chat.

## Global constraints

- Preserve the existing printer edits in this checkout; work on `codex/printer-review-fixes` in place.
- Retain 100 ms control, 500 ms sensing, 2 s stale timeout, disabled-on-boot, thermal fault latching, automatic heat ownership, and maintenance/cooldown behavior.
- Preserve synchronous target-save success semantics: a successful request persists and queues its admitted target; a failed save must not change the live target. Concurrent operations must not lose an accepted update.
- Use the existing per-boot CSRF token and Origin policy, including setup AP access; update browser clients and API tests together.
- Keep the 256-byte target body limit and reject deep input before recursive parsing.
- No hardware writes, flashing, signing, publication, dependency upgrades, or commits.

## Review focus

- A target save stalls while Disable, sensor disconnect, over-temperature, or stale detection needs the control loop.
- Maintenance, queue pressure, another target setter, or automatic acquisition races an admitted target save.
- Chamber requests have missing/mismatched token or Origin, including normal dashboard and setup AP requests.
- Target JSON is deeply nested, duplicated, trailing, malformed, or arrives incompletely.
- Wi-Fi initialization fails after allocating netifs, handlers, or radio state and is retried more than once.

### Task 1: Keep safety processing independent of persistence

**Files:** `components/runtime/app_runtime.c`, `test_apps/host/test_runtime_maintenance.c`, relevant runtime unit tests if necessary.

- [x] Add regressions that block the persistence callback and assert actual output shuts off for Disable and sensor/stale faults before releasing the callback; observe failure on current code.
- [x] Reserve target admission and command capacity under the short lock, serialize setters independently, and persist outside the controller lock. Prevent automatic acquisition until the admitted target is applied. Keep maintenance inhibition immediate.
- [x] Verify failed persistence, concurrent setter/queue pressure and maintenance cleanup preserve target consistency; run runtime and full host tests.

### Task 2: Harden chamber HTTP and retain SSE uptime

**Files:** `components/web_server/web_server.c`, `api_contract.c`, existing HTTP policy/helper interfaces as needed, `web/app.js`, host/web/on-target tests and API documentation as needed.

- [x] Add failing behavioral regressions for deeply nested target input, stalled request body, missing/invalid Origin or CSRF, and emitted SSE uptime.
- [x] Reject recursive/invalid target shapes before cJSON, bound body receive lifetime, reuse the Origin/CSRF gate on all chamber mutations, and send the token from the dashboard.
- [x] Preserve the real status snapshot when generating comparable SSE status.
- [x] Verify legitimate actions, setup AP access, malformed inputs and command errors; run focused host and browser tests. Coordinate host CMake additions with the controller.

### Task 3: Make Wi-Fi initialization retry-safe

**Files:** `components/wifi_manager/wifi_manager.c`, `test_apps/host/test_wifi_manager.c` and radio startup fixtures as needed.

- [x] Add a failure-injection regression for a post-netif startup failure followed by retries; observe the duplicate initialization failure.
- [x] Track completed stages or unwind partial resources, including event handlers, without duplicating default netifs on retry.
- [x] Verify initialization failures and the existing disconnect/reconnect scenarios. Coordinate host CMake additions with the controller.

### Task 4: Integrate and verify

- [x] Review all new changes against the initial working-tree snapshots and the six accepted findings.
- [x] Run all host C tests, browser tests, packaging tests, the production ESP-IDF build, and compiled printer stack check. Compile on-target tests where changed.
- [x] Obtain an independent review of the final changes, resolve important defects, and verify the affected checks again.
- [x] Report the fixes, test results and physical verification limits.
