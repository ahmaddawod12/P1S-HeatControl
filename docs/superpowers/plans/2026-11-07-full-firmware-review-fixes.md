# Full Firmware Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Fix the seven confirmed whole-firmware review findings with regressions.

**Architecture:** Preserve existing state machines and APIs. Correct release
validation, event filtering, SSE resource ownership/initialization, and socket
mode scope at their existing component boundaries.

**Tech Stack:** ESP-IDF 6.1, C11, MinGW/CTest, Python unittest, Node test, PowerShell.

**Spec:** `docs/superpowers/specs/2026-11-07-full-firmware-review-fixes-design.md`

## Global Constraints

- Keep ESP-IDF 6.1, active-high GPIO16/GPIO17 and the fixed 4 MB OTA layout.
- Preserve heater regulation, freshness checks, cancellation and send deadlines.
- Keep automatic Resume default-off and preserve existing public API fields.
- Use generated fixture keys only in tests; do not flash, publish or rotate keys.
- Leave the changes reviewable in the current clean checkout.

## Review Focus

- A stale build or changed key file must not hide an incompatible embedded key.
- IP loss without radio disassociation must open fallback and later recover.
- Irrelevant hotspot events must leave a healthy printer connection intact.
- Stream write/init failures must neither leak sockets nor corrupt responses.
- Partial receive records and failed writes must preserve SDK socket semantics.

### Task 1: Reject unsafe or incompatible signed builds

**Files:** `tools/package_firmware.py`, `test_apps/tools/test_package_firmware.py`.
**Interfaces:** Existing package/verify/bootstrap entry points continue to accept
the same CLI arguments and return the same manifest.

- [x] Add signed-fixture regressions rejecting encryption in both build profiles,
  mismatched/missing verification keys and a stale embedded key. Retain successful
  generic and personal packaging with the matching ECDSA key.
- [x] Run the new regressions and record their expected failures.
- [x] Share security validation across both profiles; resolve relative configured
  keys against the project directory, compare key bytes, and check the unsigned
  application's embedded key. Reject unsupported schemes and encryption.
- [x] Run all Python packaging tests and the publisher scenarios.

### Task 2: Correct station IP event handling

**Files:** `components/wifi_manager/wifi_manager.c`,
`components/printer_link/printer_transport.c`, relevant host tests/stubs and
`test_apps/host/CMakeLists.txt`.
**Interfaces:** Keep existing Wi-Fi status and printer generation APIs.

- [x] Add native callback/worker tests for lease loss, fallback and reacquisition,
  and printer tests for ignored AP assignments versus station IP invalidation.
- [x] Run the new cases and observe the original failures.
- [x] Register/filter station GOT_IP and LOST_IP events and route loss through
  existing offline handling; filter the printer handler to station events.
- [x] Run the relevant Wi-Fi startup/recovery and printer host cases.

### Task 3: Repair SSE initialization and stream cleanup

**Files:** `components/web_server/sse_hub.c`, its header,
`components/web_server/web_server.c`, host HTTP stubs/tests and CMake.
**Interfaces:** Add a startup-only `esp_err_t sse_hub_init(void)` and call it
before HTTP startup. Retain existing add/broadcast/count/close APIs.

- [x] Add regressions for first-use lock initialization, header/body send
  failures, socket closure, cleanup and subsequent stream reconnection.
- [x] Run the new cases and observe the missing close/startup initialization.
- [x] Initialize a single static mutex synchronously before accepting HTTP;
  fail startup if unavailable. On stream release shut down its socket before
  completing the async request, preserving SDK ownership of final close.
- [x] Run SSE tests plus existing real-handler and dashboard cases.

### Task 4: Scope nonblocking mode to bounded printer writes

**Files:** `components/printer_link/printer_deadline_transport.c`, its header,
`printer_socket.c`, host transport fakes and deadline/receive tests.
**Interfaces:** Keep the native deadline transport contract; make the internal
socket helper save and restore the original socket flags.

- [x] Add tests showing connect/read preserve normal socket mode, fragmented
  receives complete, and writes restore flags after success, I/O error, deadline
  expiry and cancellation. A restore failure must invalidate the connection.
- [x] Run the new cases and observe the global nonblocking-mode regression.
- [x] Remove the connect-time mode change. Apply nonblocking mode only around
  bounded inner writes and restore flags on all exits; retain existing absolute
  deadline and guard checks.
- [x] Run deadline, printer integration and receive regressions.

### Task 5: Integration verification and independent review

- [x] Run complete CTest, Node dashboard, Python packaging and PowerShell
  publisher suites; build release and Unity images; check compiled HTTP stack.
- [ ] Review the complete source/test diff with a fresh reviewer; address
  actionable findings with failing regressions before implementation changes.
- [ ] Confirm source diff scope and report outcomes and hardware-test limits.
