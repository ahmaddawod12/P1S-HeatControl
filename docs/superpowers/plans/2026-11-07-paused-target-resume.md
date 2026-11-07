# Paused Target Resume Implementation Plan

> **Superseded (2026-11-08)** by the [Monitor P1S design](../specs/2026-11-07-monitor-p1s-design.md).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** A deliberate click on a live paused printer preheats only until target, then sends one Resume without job-history prerequisites.

**Architecture:** Keep the existing coordinator, heat lease and MQTT deadline
dispatcher. Make the same paused-state and temperature-threshold policy apply
at admission, during preheat and immediately before native dispatch.

**Tech Stack:** ESP-IDF 6.1/C, host CTest, Node browser tests, Python packaging.

**Spec:** `docs/superpowers/specs/2026-11-07-paused-target-resume-design.md`

## Global Constraints

- Live/fresh paused status; no identity/startup/layer/confirmation prerequisite.
- Resume immediately at filtered temperature >= target; no settling delay/band.
- One send per deliberate cycle; no automatic retry or reconnect replay.
- Preserve Stop, sensor/heater limits, ownership and absolute dispatch deadline.
- Preserve the existing dirty checkout, Wi-Fi/printer configuration and no-reset upload requirement.

## Review Focus

- Optional identity enrichment must not revoke an offered token or active cycle.
- A hidden pause/run/pause transition must revoke an old cycle, while a new explicit click can arm the new pause.
- A last-moment temperature drop, disconnected link or Stop must prevent a wire send.
- An uncertain first send must never be silently retried or duplicated by another browser.
- Old cached clients may include the obsolete confirmation field without restoring its gate.

### Task 1: Paused-state and target-threshold policy

**Files:** `components/printer_control/printer_cycle.c`, `.h`,
`printer_control.c`, `components/printer_link/printer_status.c`,
`printer_link.c`, `components/app_types/include/printer_types.h`,
and their host/Unity fixtures.

1. Add failing cold-paused admission and below/equal/above-target tests; run
   the focused host targets and confirm behavioral assertion failures.
2. Remove job/startup admission and settling state. Bind request tokens and
   active cycles to connection/run/pause generations, not optional identities.
3. Apply the paused-state predicate at native dispatch; retain final heat,
   ownership, Stop and send-deadline checks.
4. Update old workflow fixtures to the approved threshold and paused contract;
   run all host tests. Expected: all tests pass, including real native dispatch.

### Task 2: Single-click dashboard/API

**Files:** `web/index.html`, `web/printer.js`,
`components/web_server/printer_api_policy.c`, `printer_types.h`,
host API and Node dashboard tests.

1. Add failing tests for a direct click without a checkbox and a token-only arm
   body; confirm failures before editing production UI/API.
2. Remove the initial-pause checkbox and requirement. Accept the obsolete
   optional boolean only for cached-client compatibility, without gating.
3. Keep offline/stale/nonpaused/double-click guards. Explain target waiting and
   threshold Resume in current user-facing copy.
4. Run all browser and API/host tests. Expected: complete suites pass.

### Task 3: Build, review and delivery evidence

**Files:** Current README/commissioning/startup/qualification instructions,
this spec and the ignored task ledger/build artifacts.

1. Update current instructions, marking old startup/settling rules superseded.
2. Run complete host, Node and package tests; compile ESP32 and Unity images;
   validate generic hardware/security settings, signed image and slot size.
3. Obtain one fresh independent review of this change against the spec and
   review focus. Address important findings with failing regressions first.
4. Record exact results and source-vs-installed state. Only upload when already
   in bootloader mode using no-reset connection/close; preserve settings and
   verify flash before claiming installation.
