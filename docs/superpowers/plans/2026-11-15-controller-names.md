# Controller Names Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users choose `p1s-<name>.local` in setup, with numbered collision recovery and a persistent final name.

**Architecture:** Extend the saved Wi-Fi configuration with a controller name, retaining legacy records. A pure naming helper produces validated names; the existing worker owns radio and mDNS updates. The dashboard consumes the actual identity through `/api/wifi`.

**Tech Stack:** ESP-IDF 6.1, C, NVS, espressif/mdns, existing HTML/CSS/JavaScript, CTest and Node.

**Spec:** `docs/superpowers/specs/2026-11-15-controller-names-design.md`

## Global Constraints

- Hostnames use `p1s-`, without `heat`.
- Names permit 1–22 lowercase letters, digits, or internal hyphens after ASCII lowercase/whitespace normalization; empty starts at `p1s-1`.
- Legacy Wi-Fi credentials survive upgrading; failed saves retain previous credentials and name.
- Collision fallback tries `p1s-1`, `p1s-2`, etc.; persist and return the actual name after probing. Keep IP access and `P1S-Heat-Setup` unchanged.
- Preserve existing CSRF/origin restrictions, operation admission, and AP recovery behavior.

## Review Focus

- Duplicate custom names after joining a different network must publish the fallback address.
- Changing only the name must work without losing the Wi-Fi credentials.
- Reconnecting or changing networks must restart name confirmation before saving the final name.
- Polling and a lost save reply must preserve drafts and usable recovery guidance.
- An unrelated board's `.local` hostname must never pass origin validation.

### Task 1: Persist and validate controller names

**Files:** device_config header/codec and new device_name helper; Wi-Fi credentials/engine; host configuration and API parsing tests; component and host CMake files.

**Interfaces:** `device_wifi_credentials_t.controller_name[23]`; `device_name_normalize`, `device_name_valid_n`, `device_hostname`, `device_name_next`; status `controller_name`, `hostname`, `requested_name`, `name_pending`.

- [x] Add parsing tests accepting named setups and rejecting invalid/duplicate fields; run `wifi_api_policy` and observe the named setup fail.
- [x] Add storage tests for named save, failed commit, blank reset, legacy `wifi_v1` migration, malformed name and future record preservation.
- [x] Implement the pure helper, one-record name/credential persistence, request parsing, and engine propagation. Keep two-field legacy requests valid with an empty name.
- [x] Build host tests and run configuration, Wi-Fi parser, and engine tests; expect all pass.

### Task 2: Apply actual board identities

**Files:** wifi_manager; Wi-Fi API; firmware origin policy/API; app_main; radio/mDNS host stubs and startup/worker/HTTP policy tests.

**Interfaces:** consume Task 1's name helper and status fields. `firmware_request_origin_valid` additionally accepts this board's current `.local` hostname.

- [x] Add startup and worker scenarios asserting numbered default, custom name application, numeric collision fallback, stable confirmation, later collisions, and failed save behavior.
- [x] Add origin tests accepting the board's chosen hostname and rejecting other `p1s-` names.
- [x] Apply saved/requested names after successful storage; synchronize actual mDNS hostname in the existing worker and API. Keep startup retries idempotent and the existing setup hotspot unchanged.
- [x] Run all host tests; expect no failures.

### Task 3: Setup name field and recovery links

**Files:** web/index.html, web/wifi.js, test_apps/web/test_wifi_dashboard.cjs, README and Wi-Fi documentation.

**Interfaces:** consume `/api/wifi` identity fields. Submit optional `controller_name` with Wi-Fi credentials.

- [x] Add tests for saved name/draft retention, live address preview, blank default, invalid name rejection, name-only saves, duplicate actual hostname, and recovery after a lost reply; observe failure before UI implementation.
- [x] Add the labelled optional field with associated help/error/preview text; reuse current styling. Make discovery and recovery links dynamic.
- [x] Run all Node tests and the complete host suite; expect all pass.
- [x] Update setup instructions, build generic firmware and the unit application, and inspect the final diff with a fresh reviewer. Record hardware checks that cannot run here.

## Verification (2026-11-15)

- Final serial CTest after follow-up fixes: **141/141 pass**; Node dashboard tests: **63/63 pass**.
- Generic ESP32 firmware and on-device unit-test application build successfully with ESP-IDF 6.1.
- Compiled HTTP Monitor P1S stack check passes (1840-byte requirement, 4096-byte task stack).
- A fresh reviewer identified three Important issues: incumbent ownership, late setup recovery, and mDNS startup blocking connectivity. All were reproduced and fixed with regression tests; the complete suites passed afterward.
- `git diff --check` passes. Existing unrelated user edits are retained.
- Physical two-board multicast behavior, simultaneous setup and restart persistence still require hardware qualification. No board was flashed and no release was published.

Execution stays in the current checkout on `codex/controller-names`, preserving
the user's existing unrelated edits. Leave changes reviewable without a commit,
push, flash, or release. The user already approved implementation in chat.

## Follow-up review fixes (authorized 2026-11-15)

- [x] Reproduce polling during a pending PUT and delivery of that old GET after the PUT settles; retain the requested recovery hostname and drafts, including rejected saves.
- [x] Capture status during a reconnect and late-conflict ownership query; assert the API exposes a pending name instead of the internal temporary hostname.
- [x] Preserve the pending Save until a GET started after its PUT can reconcile it. Keep public hostname state owned by the Wi-Fi worker and publish pending state before preflight.
- [x] Explicitly verify that an unnamed connected board starts at `p1s-1` and saves `p1s-2` if the first name is occupied.
- [x] Run the complete host and dashboard suites, rebuild generic/unit firmware, and check the final diff. Keep the existing hotspot and unrelated edits intact.

Both review regressions failed before the fixes and passed afterward. All
141 host tests and 63 dashboard tests pass. Generic and unit ESP32 firmware
rebuilt successfully; compiled HTTP stack check and `git diff --check` pass.
Physical two-board testing remains pending.
