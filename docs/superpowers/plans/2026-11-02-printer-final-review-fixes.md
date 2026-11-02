# Printer Final Review Fixes Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans and test-driven-development. Preserve the current checkout changes and leave all fixes uncommitted.

**Goal:** Fix the three reproduced review findings: intervening safety history ignored at Resume, incomplete identity suppressing snapshots, and replacement drafts lost after Forget.

**Architecture:** Capture the armed safety history with the dispatch and pass it through the native callback to the final link validation. Preserve the initial snapshot and retry incomplete active-job metadata within the existing attempt budget. Clear a forgotten draft only if it still matches the draft submitted with DELETE.

**Tech Stack:** C / ESP-IDF 6.1, ESP-MQTT, MinGW CTest fixtures, vanilla JavaScript / Node browser fixtures.

**Spec:** The three findings approved by “Fix these issues”; `docs/superpowers/specs/2026-11-01-printer-preheat-resume-design.md`, sections 5–8 and 10.

## Global Constraints

- Preserve the existing changes and index; reuse this checkout on `codex/printer-review-fixes`.
- Snapshot attempts remain limited to one per 300 seconds, including failures and reconnects.
- Resume remains QoS 0, nonretained, one attempt, with a maximum 1,000 ms send deadline.
- Keep automatic Resume disabled by default and preserve independent five-second heat leases.
- No board/printer connection, flash, deployment, or publication.

## Review Focus

- Error/HMS/identity transitions that clear before sending must still reject Resume.
- Reports queued after coordinator refresh must be checked by the link itself.
- Benign metadata/temperature updates may still release the healthy captured job.
- Idle status without job identity must not cause repeated unnecessary snapshots.
- Forget must clear an unchanged draft while retaining newer fields, including an access-code-only edit.

### Task 1: Bind final Resume to armed history

**Files:** `components/printer_control/{printer_control.c,printer_control_native.c,include/printer_control.h}`, `components/printer_link/{printer_link.c,include/printer_link.h}`, affected host/Unity fixtures and `test_apps/host/CMakeLists.txt`.

**Interface:** Add `const printer_safety_history_t *expected_history` after the job parameter in `printer_link_resume_once` and `printer_control_ops_t.link_resume`. Capture `s.cycle.armed_history` under the coordinator lock; reject a mismatching history after the link drains queued reports.

- [x] Add real coordinator/link/native-adapter regressions for transient error, HMS and identity changes both before coordinator refresh and immediately before link validation; include a benign update case.
- [x] Run the new CTest cases. Expected: adverse-history cases FAIL because Resume is published.
- [x] Implement history propagation and final validation; update native/Unity fixture callback signatures.
- [x] Run all printer host cases. Expected: all PASS, with no Resume after adverse history and normal Resume after benign updates.

### Task 2: Recover missing job identity

**Files:** `components/printer_link/{printer_link.c,printer_status.c,printer_status.h}`, `test_apps/host/{test_printer_link.c,CMakeLists.txt}`.

**Interface:** Add a status predicate for required snapshots that includes missing decision fields or missing identity in an active job; share the identity-validity calculation with snapshot projection.

- [x] Add cases for partial metadata arriving with SUBACK, cooldown-delayed identity recovery, and idle status without identity.
- [x] Run those CTest cases. Expected: missing-identity cases FAIL because no snapshot is requested.
- [x] Preserve the first snapshot attempt and use the status predicate for later retries, retaining the 300,000 ms budget.
- [x] Run all link cases. Expected: all PASS; complete/idle status suppresses unnecessary later attempts.

### Task 3: Preserve replacement drafts during Forget

**Files:** `web/printer.js`, `test_apps/web/test_printer_dashboard.cjs`.

**Interface:** Preserve existing HTTP routes and editable inputs; capture the submitted draft and access code, then use `sameDraft` before clearing it.

- [x] Add held-DELETE cases for newer fields, access-code-only edits and unchanged drafts.
- [x] Run `node --test test_apps/web/test_printer_dashboard.cjs`. Expected: changed-draft cases FAIL with erased fields.
- [x] Guard successful Forget cleanup with `sameDraft`.
- [x] Run all browser tests. Expected: all PASS.

### Final verification

- [x] Build/run the complete host suite and all browser tests; run packaging and publisher fixtures.
- [x] Rebuild generic release and native Unity images; run the HTTP arm stack check and `git diff --check`.
- [x] Obtain one independent read-only review of the fixes and resolve concrete findings.
- [x] Record results in `docs/printer-integration-qualification.md` and the ignored execution ledger.
