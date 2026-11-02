# Printer Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Fix the two reproduced P2 issues from the review of `0df81a6`.

**Architecture:** Retry a missing MQTT baseline from regular link ticks, using the existing 300-second snapshot-attempt budget. Capture runtime inputs while holding the coordinator lock so successful arm admission cannot invalidate the poll's ownership snapshot.

**Tech Stack:** C, ESP-IDF 6.1, ESP-MQTT, MinGW/CTest host fixtures.

**Spec:** `docs/superpowers/specs/2026-11-01-printer-preheat-resume-design.md`, sections 5 and 8; the two reproduced findings in this chat.

## Global Constraints

- Snapshot attempts remain limited to one per 300 seconds, including reconnects and failed writes.
- Preserve baseline invalidation, independent heat-lease expiry, Stop/Disable, and the default-off automatic Resume gate.
- Reuse the current checkout. Leave changes uncommitted for review.

## Review Focus

- A new run with omitted unchanged fields eventually requests and recovers its baseline.
- A reconnect inside the cooldown retries when the cooldown expires while staying subscribed.
- A baseline rebuilt by deltas suppresses unnecessary snapshot requests.
- Failed snapshot attempts consume the cooldown budget.
- An arm admitted between telemetry collection and policy processing keeps its valid heat lease; existing Stop and expiry cases still pass.

### Task 1: Recover missing baselines

**Files:** `components/printer_link/printer_link.c`, `components/printer_link/printer_status.c`, `components/printer_link/printer_status.h`, `test_apps/host/test_printer_link.c`, `test_apps/host/CMakeLists.txt`.

**Interfaces:** Preserve `printer_link_tick(uint64_t)` and the existing wire requests. Share the required-field baseline predicate between status projection and link recovery.

- [x] Add link-level regressions for sparse new-run reports, cooldown-delayed reconnect recovery, delta-only recovery, and failed snapshot attempts.
- [x] Build the host targets and run the new CTest cases; confirm the missing-retry cases fail before changing production code.
- [x] Move missing-baseline snapshot attempts into the regular tick path while retaining the attempt budget.
- [x] Run all printer-link cases; expect success and no extra requests once a valid baseline returns.

### Task 2: Serialize arm admission and poll inputs

**Files:** `components/printer_control/printer_control.c`, `test_apps/host/test_printer_control.c`, `test_apps/host/CMakeLists.txt`, `docs/printer-integration-qualification.md`.

**Interfaces:** Preserve coordinator APIs. Use a test-only semaphore wrapper to schedule successful HTTP-style arm admission just before the poll takes its policy lock.

- [x] Add a deterministic regression asserting that the admitted cycle remains preheating with an active lease and retained service ownership.
- [x] Run `printer_control`; confirm the new assertion fails with the original snapshot ordering.
- [x] Capture runtime inputs under the coordinator lock and rerun the test; expect success.
- [x] Run the complete host, browser, packaging, and publisher fixture suites; build the generic firmware and native Unity image; check the HTTP stack budget and whitespace.
- [x] Obtain one independent read-only review of the final diff and address any concrete regressions.
