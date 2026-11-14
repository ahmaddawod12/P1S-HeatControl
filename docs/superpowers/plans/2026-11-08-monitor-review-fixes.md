# Monitor Review Fixes Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement these fixes in this checkout. Track each regression from failure to passing.

**Goal:** Fix the three reproduced Monitor P1S issues from the review of `4be033c`.

**Architecture:** Retain a bounded completion counter and terminal state alongside the merged printer reports. Each monitor takes a thread-safe snapshot including pending reports at start, so earlier completions are excluded and subsequent completions survive report bursts and reconnects. Serialize coordinator snapshots with session start. Clear runtime monitoring when processing Disable, and mark unrecognized printer states unknown so recovery remains active.

**Tech Stack:** ESP-IDF 6.1, C, FreeRTOS, CMake/CTest, Python, Node.

**Spec:** The three findings in this chat and `docs/superpowers/specs/2026-11-07-monitor-p1s-design.md`.

## Constraints and review focus

- Preserve bounded mailbox memory, the five-minute pushall limit, and manual heating behavior.
- Ignore completions already observed before a new monitor starts.
- Preserve an observed completion if newer reports are running, malformed, or from a reconnected link.
- Repeated terminal reports must not fabricate completions or end a later waiting session.
- Keep the existing release-workflow changes intact and leave the fixes available for review locally.

## Tasks

- [x] Add link/monitor integration regressions for coalesced start/finish, finish/new-print, malformed follow-up, reconnect, and prior-session completion history. Run them and observe the failures, then retain completion history and consume it in the monitor. Run the printer suites.
- [x] Add a deterministic runtime regression that pauses command reception, submits Disable then Enable, and checks that the old monitor ended and manual heating resumes. Observe failure, clear monitoring on Disable, and run the runtime suite.
- [x] Add an unrecognized-state recovery regression at the five-minute boundary. Observe failure, derive state_known from recognized states, and run the link suite.
- [x] Run all host, dashboard, Python and publisher tests; rebuild firmware and Unity images; run the compiled stack check, workflow lint, and whitespace check. Review the final diff independently and resolve material findings.

## Progress

- Baseline: review reproduced all three failures; 102 host, 41 dashboard, 31 Python and 10 publisher checks passed before changes.
- Execution: reuse the current checkout under the user's request to fix the reviewed code; no new worktree or integration action is needed.
- Initial regression run: all seven new CTest cases failed for the reviewed behaviors before implementation. Targeted printer, runtime and recovery suites passed after their fixes.
- Independent review found that a completion pending before Monitor was pressed could incorrectly end the new session. Added link and coordinator regressions, observed both fail, then made link snapshots thread-safe and current and serialized them with coordinator start. Both regressions pass.
- Final verification: 110/110 host tests, 41/41 dashboard tests, 31/31 Python tests and all 10 publisher scenarios passed. Release and Unity images rebuilt. Actionlint and whitespace checks passed.
- Compiled stack validation: existing HTTP paths require at most 1840/4096 bytes, including the configured margin. The added live link-snapshot wait/merge paths require 1744/1712 bytes and fit within that bound.
- Hardware tests and live GitHub Actions execution were not performed. Fixes remain local for review.
