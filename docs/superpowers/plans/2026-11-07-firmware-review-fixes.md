# Firmware Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Fix HTTP starvation, misleading fault acknowledgements and empty diagnostic history.

**Architecture:** Share bounded HTTP acquisition and connection-closing helpers.
Keep acknowledgement decisions and transition recording in the controller task.
Use existing host transport fakes and threaded runtime tests for regression coverage.

**Tech Stack:** ESP-IDF 6.1, C, FreeRTOS, CMake/CTest, Node.js and Python.

**Spec:** `docs/superpowers/specs/2026-11-07-firmware-review-fixes-design.md`

## Global Constraints

- Preserve existing working-tree changes and current control policy.
- HTTP body acquisition has a five-second absolute deadline.
- Fault refusal is 409; unavailability is 503; successful acknowledgement is 204 and never enables heat.
- No hardware flashing or changes to network credentials.

## Review Focus

- Unauthorized and oversized bodies must close without draining unread input.
- A slow final chunk must not extend the absolute receive deadline.
- An acknowledgement delayed past its caller's deadline must not clear a fault.
- A new fault observed during acknowledgement must be reported as refusal.
- Stable faults must not flood the 32-record history, and recording must not block safety.

### Task 1: Bound HTTP requests

**Files:** Create `components/web_server/http_request.{c,h}`; modify the four
handler sources, component CMake, and `test_apps/host/test_chamber_http.c`, its
transport stubs and host CMake.

**Interfaces:** Produce `http_request_receive_body(httpd_req_t *, char *, size_t)`
and `http_request_close_json(httpd_req_t *, const char *, const char *)` returning
`esp_err_t`; existing route callbacks and JSON contracts remain unchanged.

- [x] Add handler regressions for all mutation routes: trust/size rejection,
  timeout/trickle/slow final chunk, embedded NUL, valid input and recovery.
- [x] Run the new CTest cases and observe failures against the current handlers.
- [x] Implement shared acquisition with a 5,000 ms deadline and close unread errors.
- [x] Run all chamber HTTP cases and verify success and timeout responses.

### Task 2: Return actual acknowledgement decisions

**Files:** Modify `components/runtime/app_runtime.c`, its public header,
`components/web_server/web_server.c`, host runtime/HTTP tests and unit tests.

**Interfaces:** Preserve `esp_err_t app_runtime_ack_fault(void)`; return
`ESP_ERR_NOT_ALLOWED` for a controller refusal and `ESP_OK` only after the
controller has applied and published a successful acknowledgement.

- [x] Add threaded regressions for output-init, unresolved sensor/stale/overtemp,
  successful recovery, delayed expiry and a changed fault during acknowledgement.
- [x] Run them and observe the incorrect success before implementation.
- [x] Add bounded request/reply correlation without holding the admission lock
  while waiting; refuse expired requests and map refusal to HTTP 409.
- [x] Run runtime and HTTP regressions; confirm Disable remains responsive.

### Task 3: Record controller transitions

**Files:** Modify runtime and component CMake; link real diagnostics into host
runtime tests using the host semaphore adapter; extend runtime regression cases.

**Interfaces:** Consume existing `diagnostics_reset()`, `diagnostics_record()`
and `diagnostics_snapshot()`; no public diagnostic format change.

- [x] Add a runtime test asserting boot/enable/fault/recovery history and no
  duplicate records while faulted, including a fault change within FAULT.
- [x] Run it and observe an empty history.
- [x] Initialize diagnostics before task creation and record state/fault changes.
- [x] Run the complete host suite and inspect history assertions.

### Task 4: Verify and review

- [x] Run full host, dashboard, packaging and publisher suites.
- [x] Build ESP32 release and unit images and run `tools/check_printer_stack.py`.
- [x] Obtain an independent review of this task's changes and address substantive findings.
- [x] Run `git diff --check`, update the plan/ledger and summarize results.

Changes remain uncommitted for review alongside the user's existing work.
