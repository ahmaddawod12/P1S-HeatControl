# Wi-Fi Fallback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Configure Wi-Fi immediately from the existing dashboard and recover through a fallback hotspot without reflashing.

**Architecture:** A host-executable Wi-Fi engine owns timing/application decisions; a single ESP-IDF worker owns SDK calls and publishes short-lock snapshots. A shared atomic service gate serializes Wi-Fi changes, OTA and real trials. Native JSON endpoints and a small dashboard controller expose the same configuration on STA and AP.

**Tech Stack:** ESP-IDF 6.1 / ESP32, C11, cJSON, FreeRTOS, vanilla JS/CSS, CTest, Unity, Node, Python packaging tests.

**Spec:** `docs/superpowers/specs/2026-10-25-wifi-fallback-design.md`

## Global Constraints

- Reuse the current feature checkout; preserve original staged migration exactly, including index SHA256 `14A72567618218F4754105F8271FDD0EE8AE000F60F145F67932CECCA98AF3DB`.
- Visible editable SSID/password, no login or password masking; same dashboard on open `P1S-Heat-Setup`, `192.168.4.1/24`.
- AP fallback at 30,000 ms; success grace 10,000 ms; save-response grace 250 ms; retries 1,2,4,8,30 seconds indefinitely.
- No restart on Save, no auto-resume heat, no network-loss effect on regulation, no credential logging or compiled credentials in public assets.
- Wi-Fi `wifi_v1` / settings schema / signing key / partition layout remain compatible; no eFuse changes.
- Core implementation remains inline; one fresh whole-change reviewer at the end. Publish/PC-network changes require concrete authorization.

## Review Focus

1. Whitespace, UTF-8 byte limits, quotes/backslashes, duplicate keys and embedded NUL must preserve exact valid credentials and reject malformed ones (Tasks 1,4,5).
2. Repeated association failures must not postpone fallback indefinitely; a late disconnect must cancel AP shutdown (Task 2).
3. Flash/AP/driver failures and rapid replacement requests must leave a reachable configuration path, retain correct saved data and release admission (Tasks 1,2).
4. Wi-Fi mutation racing OTA preflight/trial must admit only one owner; an initial setup without a rollback image must still accept credentials after 120 s (Tasks 1,3,4).
5. A lost HTTP reply or polling during edits must not send duplicate saves, overwrite drafts or discard reconnect instructions (Task 5).

## Verification commands

Use the plan workspace `.superpowers/sdd/2026-10-25-wifi-fallback-implementation` for logs and a fresh `host-build` cache. Configure/build with `C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe`, `-G "MinGW Makefiles"`; run its `ctest.exe --test-dir <workspace>/host-build --output-on-failure`.

Browser: `node --test test_apps/web/*.cjs`. Packaging: SDK Python `-m unittest discover -s test_apps/tools -p test_package_firmware.py`. Publisher: `./test_apps/tools/test_publish_firmware.ps1` (fake CLI only).

Use the activated installed ESP-IDF 6.1 SDK Python for main/unit/bootstrap builds and signing. USB tests use the detected known COM3 board only; close monitors first and preserve its NVS/config/OTA metadata before unit firmware.

### Task 1: Durable replacement and operation admission

**Files:** `components/device_config/{device_config.c,include/device_config.h}`, new `components/runtime/{service_operation.c,include/service_operation.h}`, runtime CMake, `test_apps/host/{test_device_config.c,test_service_operation.c,CMakeLists.txt}`, native device-config cases.

**Interfaces:** Produce `esp_err_t device_config_save_wifi(const device_wifi_credentials_t *)`, preserving seed behavior and unknown formats. Produce `service_operation_t` owners NONE/WIFI/OTA/TRIAL and `bool service_operation_begin(owner)`, `void service_operation_end(owner)`, `service_operation_t service_operation_current(void)`.

- [ ] Write `test_wifi_replace_preserves_exact_bytes_and_other_records`, `test_failed_commit_retains_previous_wifi`, `test_future_wifi_is_not_overwritten`, and actual atomic/thread race tests for one owner and wrong-owner release.
- [ ] Configure/build/run new tests; expected RED (missing interfaces, then behavioral assertion).
- [ ] Implement codec replacement and atomic admission; no automatic partition erase.
- [ ] Run host suite; expected all tests green. Commit only owned paths; keep original staged paths untouched.

### Task 2: Wi-Fi engine and ESP-IDF worker

**Files:** `components/wifi_manager/` header/CMake, new `wifi_credentials.c`, `wifi_engine.c/.h`, native `wifi_manager.c`; new host `test_wifi_engine.c`, native Wi-Fi tests.

**Interfaces:** Extend `wifi_manager_status_t` with configured/password/applying/can_save/AP fields and `wifi_result_t`. Keep existing fields and validation/backoff signatures. Produce `wifi_manager_request_credentials(const wifi_credentials_t *)`, `wifi_manager_result_name(result)`.

Engine: `wifi_engine_init(engine, credentials_or_null, storage_error, now_ms)`, `wifi_engine_apply(engine, credentials, now_ms, ops)`, `wifi_engine_tick(engine, now_ms, can_close_ap, ops)`, `wifi_engine_connected(engine, ssid, ip, rssi, now_ms)`, `wifi_engine_disconnected(engine, result, now_ms)`. Operations inject save, AP enable/disable and station reconfigure/connect with a context pointer.

- [ ] Write tests for immediate no-credentials AP; 29,999/30,000 fallback; repeated failures; 9,999/10,000 success grace; interrupted grace; stale other-SSID IP; empty password and exact whitespace; AP/save/driver failures; recover after wrong credentials; no AP shutdown during reserved operation.
- [ ] Run engine target; expected RED before implementation.
- [ ] Implement engine, then single native worker with short snapshot locks, async queued saves and response grace. Native callbacks only buffer network events. Start AP+STA without restarting the HTTP server, keep retries running, and preserve existing station runtime publication.
- [ ] Generic missing credentials are normal setup, corrupt/unknown storage remains a fault without erase. Save ensures AP before commit/disconnect and releases WIFI gate on every completed path.
- [ ] Run host suite and main build; expected green. Native SDK adapters are compiled here and physically exercised in Task 6. Commit owned files, leave pre-staged `wifi_manager.c` edits unstaged.

### Task 3: Preserve OTA and initial-setup health

**Files:** firmware update worker/trial/policy; host trial tests; native admission case.

**Interfaces:** Consume service gate from Task 1 and Wi-Fi status from Task 2. Add trial action SETUP_WAIT and `firmware_trial_evaluate_boot(health, elapsed_ms, pending, rollback_available)`; preserve the existing policy function for its current callers/tests.

- [ ] Add tests: real trial rejects at 120,000 ms; initial setup reports SETUP_WAIT then confirms after late valid health; hotspot-only health does not confirm; confirmed network outage does not roll back. Native test holds WIFI and verifies actual firmware request admission rejects it.
- [ ] Run tests; expected RED on late initial setup recovery.
- [ ] Reserve OTA through the entire worker operation and TRIAL only for an unconfirmed image with a rollback target. Initial setup continues health checks after setup-required, without restarting; keep safe boot maintenance until local health succeeds.
- [ ] Verify all gate release/failure paths and run host suite/main build; expected green. Commit.

### Task 4: Same-server Wi-Fi API

**Files:** new web-server `wifi_api.c/.h`, `wifi_api_policy.c/.h`; existing internal firmware guard/header, web-server ops/header/CMake/routes; host policy and native HTTP tests.

**Interfaces:** Optional injected `get_wifi_status(wifi_manager_status_t *)` and `request_wifi_change(const wifi_credentials_t *)` in `web_server_ops_t`; production wires Task 2. Reuse exported internal per-boot token/trusted-request helper. `GET /api/wifi`, `PUT /api/wifi`; exactly two string keys, body <=768, validation from Task 2. GET is no-store and includes credentials, status/AP fields, `can_save` and token. PUT returns 202, 400, 403, 409 or 503. Reserve at least 20 native route slots.

- [ ] Write strict parser tests (duplicate/unknown keys, escaped/raw NUL, trailing JSON, whitespace and escaped credentials) and native GET/plaintext PUT/forgery/busy/oversize cases.
- [ ] Observe RED, implement bounded handlers/guard reuse and route registration.
- [ ] Keep existing firmware-route behavior unchanged. Run host suite and build/run native tests when hardware preparation is ready; expected green, no fixture stack overflow. Commit owned files, retain pre-staged web-server/test edits unstaged.

### Task 5: Dashboard Wi-Fi section

**Files:** `web/{index.html,styles.css,app.js,CMakeLists.txt}`, new `web/wifi.js`, static route, new `test_apps/web/test_wifi_dashboard.cjs`.

**Interfaces:** Consume Task 4 API. Plain text `wifi-ssid` / `wifi-password`, button `wifi-save-button`, live `wifi-message`, AP details and normal-network link. Separate poller avoids changing firmware polling behavior. A small browser event communicates Wi-Fi application busy state to the existing firmware button.

- [ ] Write tests proving visible input type, saved fields, draft preservation, exact spaces, one PUT, immediate busy feedback, lost reply without automatic retry, failed save retains draft, bad-password recovery instructions, successful address and grace-period instructions.
- [ ] Run Node tests; expected RED before UI/controller implementation.
- [ ] Implement matching the current dark dashboard, visible labels, keyboard/touch controls, status text and reconnect instructions. Never put a password in a URL or console log.
- [ ] Run all Node tests and main build; expected green. Inspect the page with dummy credentials in a local/browser fixture; preserve actual credentials from screenshots/chat output. Commit owned files.

### Task 6: Integrated verification and delivery

**Files:** version 1.0.5, release/implementation/commissioning documentation; private test evidence only in ignored paths.

- [ ] Run all host/browser/package/publisher tests, generic/bootstrap/unit builds, signed generic validation and publication dry run. Expected all green; the public package has only the generic binary/manifest and no credentials.
- [ ] Dispatch the fresh whole-change reviewer with this plan/spec/ledger and original-index constraints. Fix Important/Critical findings through reproducing RED→GREEN and full green suites; ledger deferred minors. No repeated review loop.
- [ ] Preserve board storage, execute native tests, restore known confirmed baseline and settings, and prepare concrete signed 1.0.5 assets before asking for public publication approval. Keep source private.
- [ ] With authorized update path and coordinated client access, measure actual AP missing/wrong-credential recovery, visible same page, immediate Save without reset, correct STA rejoin, AP shutdown, persistence, four SSE/API responsiveness and OTA exclusion. Report any hardware checks not executed honestly.
- [ ] Verify original staged hash unchanged, retain complete working-tree edits and private evidence, and commit owned implementation/docs. No source push/PR unless requested.
