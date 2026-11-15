# Relay Fault Lockout and Printer Save Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement these authorized fixes inline. Track each verification below.

**Goal:** Keep relay initialization faults latched and preserve printer Save drafts across pre-acceptance polling races.

**Architecture:** Guard the existing controller fault latch and reuse the dashboard's request-timing checks. Keep APIs and persistence schemas unchanged.

**Tech Stack:** ESP-IDF 6.1, C, Unity/host tests, JavaScript/Node tests.

**Spec:** `docs/superpowers/specs/2026-11-14-fault-lockout-printer-save-design.md`

## Global Constraints

Work in the current checkout and preserve the existing `version.txt` edit. No commit, flash, signing or publication.

## Review Focus

- Both orders of output/NVS faults must keep output initialization faults fatal.
- Disable, target updates and repeated fault reports must not reopen heating.
- NVS-only acknowledgement must still recover normally.
- A GET started during Save cannot confirm it, even after acceptance arrives.
- Failed persistence and edits made during Save must retain their draft.

### Task 1: Preserve the relay fault lockout

**Files:** `components/controller/heat_controller.c`, `test_apps/host/test_heat_controller.c`, `test_apps/unit/main/test_heat_controller.c`.

**Interfaces:** Existing `heat_controller_command` and snapshot; no new interface.

- [x] Add a controller regression for OUTPUT_INIT -> NVS -> Acknowledge -> Enable; assert the fault remains OUTPUT_INIT and heating stays disabled. Mirror in Unity.
- [x] Build the host test and run `ctest --test-dir test_apps/host/build -R '^heat_controller$' --output-on-failure`; expect the new assertion to fail before the fix.
- [x] Preserve OUTPUT_INIT in the shared fault latch; retain current handling of other faults.
- [x] Repeat the controller test; expect PASS, including existing NVS-only recovery.

### Task 2: Preserve printer Save drafts

**Files:** `web/printer.js`, `test_apps/web/test_printer_dashboard.cjs`.

**Interfaces:** Existing PUT and status response; no new interface.

- [x] Add delayed-PUT support to the browser fixture and regressions for old status before rejection and old GET delivery after acceptance; assert the code is retained until fresh confirmation and requests are not repeated.
- [x] Run `node --test test_apps/web/test_printer_dashboard.cjs`; expect the new access-code retention assertions to fail before the fix.
- [x] Apply Save's request-start/current-save timing guards before resolving its pending state, as for Forget.
- [x] Repeat the printer dashboard tests; expect PASS, including eventual successful clearing and newer-draft preservation.

### Final verification

- [x] Run all host, dashboard, Python tooling and publisher safeguard tests.
- [x] Build generic ESP32 firmware and the unit application; run the compiled stack check.
- [x] Obtain an independent read-only review and address actionable findings.
- [x] Check the final diff and confirm the original version edit is preserved.

## Completion evidence

- The new host safety regression and both Save timing regressions failed before
  the production fixes, then passed afterward.
- All 124 host tests, 52 dashboard tests, 31 Python tooling tests and 10
  publisher safeguard cases passed.
- The generic ESP-IDF 6.1 firmware and the unit application built successfully.
  The new Unity regression compiled; it was not run on hardware.
- Compiled printer HTTP stack analysis passed: 1,840 required bytes, including
  margin, within the configured 4,096-byte stack.
- Independent read-only review found no production correctness defects. Its
  Save coverage observation was addressed with persistence-failure/retry and
  newer-draft compatibility cases; both passed.
- `git diff --check` passed. The pre-existing `version.txt` edit is preserved.
  No commit, flash, production signing or publication was performed.
