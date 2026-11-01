# GitHub OTA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the owner press Update firmware in the local dashboard to download a signed public GitHub release, install it with heating locked off, and recover the previous firmware if trial startup fails.

**Architecture:** Add a dedicated device-configuration store and firmware-update component alongside the existing runtime. The runtime owns the maintenance interlock; a worker streams HTTPS data into the inactive application slot and selects it only after verification. An independent startup supervisor confirms the trial application after local services recover, with the dashboard polling a separate update-status endpoint.

**Tech Stack:** C, ESP-IDF 6.1, classic ESP32, FreeRTOS, NVS, esp_http_client, esp_ota_ops, mbedTLS, managed cJSON, existing embedded HTML/CSS/JavaScript, Python standard-library release tooling, Unity and existing CMake host tests.

**Spec:** [Approved GitHub OTA design](../specs/2026-10-25-github-ota-design.md).

## Global Constraints

- Classic ESP32-WROOM-32, ESP-IDF 6.1, 4 MB flash, UART0 console.
- Wi-Fi and temperature regulation remain independent of internet access outside explicit update checks.
- Public compiled firmware lives in a separate repository; `ahmaddawod12/P1S-HeatControl` remains private. Proposed public repository: `ahmaddawod12/P1S-HeatControl-Releases`, subject to availability.
- Each application slot is 2,031,616 bytes. Use the spec's exact partition offsets; verify bootloader fit below the partition table at `0x8000` before the initial USB migration.
- One initial USB installation is required. Never send bootloader, partition-table, or NVS replacement images through the updater.
- Use ECDSA version-1 update signatures without enabling hardware secure boot, eFuse anti-rollback, or disabling serial recovery.
- Heating always starts disabled. Finish the 10-second disable cooldown before writing application flash; keep sampling and safe fault handling alive.
- Trial validation has a 120-second deadline and requires recovered local Wi-Fi and HTTP service. Internet, GitHub, mDNS, and an open browser are not confirmation prerequisites.
- No automatic installation, periodic external update checks, prereleases, browser firmware uploads, caller-selected URLs, GitHub token on the device, or general dashboard account system.
- Preserve the existing staged work. Commit only each task's changes; never run a blanket add/reset/stash or include unrelated staged hunks.
- On-target Unity tests replace application firmware and erase their test configuration. Run them before the final personal bootstrap; reflash the bootstrap afterward.

## Review Focus

1. A stale browser retries an accepted POST after losing its response: one worker must own the attempt; a reboot must not accidentally trigger another update. Exercise in Tasks 5 and 7.
2. Enable or target-save races the transition into maintenance: no queued command may energize the heater or modify persisted target after the lock is accepted. Exercise in Task 3.
3. Cold boot has no usable clock, or GitHub redirects/rate-limits the request: preserve certificate checking, bound work, and leave normal heating unchanged when preflight fails. Exercise in Tasks 4 and 5.
4. The update encounters an existing physical sensor fault or an exhausted SSE socket pool: a safe, responsive local service must remain recoverable without internet or a browser. Exercise in Tasks 6 and 7.
5. Power fails between recording an attempt, selecting the next slot, and confirming it: retain credentials, identify the running version correctly, and never claim success from the stored attempt alone. Exercise in Tasks 5, 6, and 8.

## File and dependency map

| Files | Responsibility |
| --- | --- |
| `components/app_types/include/firmware_types.h` | Stable bounded identity, manifest/status, and attempt-record types shared by storage, runtime-facing updater, and web code; no SDK dependency |
| `components/device_config/{device_config.c,include/device_config.h,CMakeLists.txt}` | Named `device_cfg` NVS access, Wi-Fi seeding/loading, additive attempt records |
| `components/firmware_update/{firmware_policy.c,firmware_manifest.c,firmware_identity.c}` | Version/compatibility rules, bounded GitHub/manifest parsing, signed image identity |
| `components/firmware_update/{firmware_transport.c,firmware_update.c,firmware_trial.c}` | HTTPS/native OTA adapter, single worker, trial supervisor respectively |
| `components/firmware_update/include/firmware_update.h` | Public updater/status/trial API |
| `components/firmware_update/{firmware_policy.h,firmware_manifest.h,firmware_transport.h,firmware_update_internal.h,firmware_trial.h}` | Internal module and injected-test interfaces |
| `components/firmware_update/{CMakeLists.txt,Kconfig.projbuild}` | SDK dependencies, fixed release repository, limits, verification key and build identity integration |
| `components/runtime/{app_runtime.c,include/app_runtime.h,runtime_output_policy.c,runtime_output_policy.h}` | Runtime-owned interlock and acknowledgements |
| `components/wifi_manager/{wifi_manager.c,include/wifi_manager.h,CMakeLists.txt}` | Load saved credentials before considering a local bootstrap seed |
| `components/app_startup/{app_startup.c,include/app_startup.h,CMakeLists.txt}`, `main/{app_main.c,CMakeLists.txt}` | Relay-first startup, storage errors, early trial supervision, updater initialization |
| `components/web_server/{web_server.c,include/web_server.h,firmware_api.c,firmware_api.h,CMakeLists.txt}`, `web/{index.html,app.js,styles.css}` | Separate firmware routes/JSON/CSRF and the update control |
| `partitions.csv`, `sdkconfig.defaults`, `CMakeLists.txt`, `version.txt`, `config/ota_signing_public.bin`, `.gitignore` | Flash layout, signing/generic-build settings, version and public key |
| `tools/{package_firmware.py,install_bootstrap.ps1,publish_firmware.ps1}` | Validated generic release package, explicit initial USB installation, explicit publication |
| `test_apps/host/{test_firmware_policy.c,CMakeLists.txt}` | Pure version/compatibility tests |
| `test_apps/unit/{partitions.csv,sdkconfig.defaults,main/CMakeLists.txt}` and new `main/test_{device_config,firmware_manifest,firmware_transport,firmware_update,firmware_trial}.c` | Device and orchestration boundaries with fake network/OTA operations |
| Existing `test_apps/unit/main/test_{runtime,wifi_config,app_startup,web_server,web_assets}.c` | Regressions through the real runtime/startup/HTTP seams |
| `test_apps/tools/test_package_firmware.py`, `test_apps/web/test_firmware_dashboard.cjs` | Packaging and consequential browser behavior |
| `README.md`, `docs/commissioning-checklist.md`, `docs/ota-release-guide.md` | Setup, signing/publication and measured hardware recovery results |

Use `app_types` for shared records to avoid a `device_config`/`firmware_update` dependency cycle. Keep `runtime` independent of `firmware_update`; the worker consumes runtime APIs. Keep native SDK operations behind the injected transport/trial operations so unit images need not actually replace themselves.

## Validation commands and execution setup

The commands below are execution instructions, not evidence that tests have already run. Activate the installed ESP-IDF 6.1 environment first (`C:/esp/v6.1/esp-idf/export.ps1` exists); ensure its tools root matches the installation and `idf.py --version` reports 6.1. The present chat shell has no active `IDF_PATH`.

Use a task-specific PowerShell variable for the selected serial port, and close any serial monitor first. Do not silently select a different connected board. The existing checkout contains staged ESP32 migration changes that are absent from HEAD: before execution, use the worktree skill and choose a workspace that retains that complete baseline. A worktree created from HEAD alone would lose those changes. Do not commit someone else's staged work to resolve isolation.

Host configure/build/run:

```powershell
$p1sCmake = 'C:/Espressif/tools/cmake/4.0.3/bin/cmake.exe'
& $p1sCmake -S test_apps/host -B test_apps/host/build -G 'MinGW Makefiles'
& $p1sCmake --build test_apps/host/build
& $p1sCmake --build test_apps/host/build --target test
```

On-target component build/run, with `$p1sPort` explicitly set to the intended board:

```powershell
idf.py -C test_apps/unit set-target esp32
idf.py -C test_apps/unit build
python -m pytest test_apps/unit/pytest_unit.py --target esp32 --embedded-services esp,idf --port $p1sPort -s
```

Each task's red/green cycle uses its named tests and these suites. A deliberate missing-symbol build failure is acceptable while adding an interface; after the interface exists, confirm the behavioral assertion fails before implementing its behavior. If no board is accessible, build the tests and record execution as unverified; do not claim on-target or power-failure checks passed.

### Task 1: Define release identity and establish the OTA build/layout

**Files:** shared `firmware_types.h`; `firmware_policy.{c,h}`; `firmware_identity.c`; firmware component registration/Kconfig; root layout/build/version/key/ignore files; host policy test/registration; unit layout/defaults.

**Interfaces:** Produce `firmware_version_t` with `uint32_t major, minor, patch`; `bool firmware_version_parse(const char *text, firmware_version_t *out)`; `int firmware_version_compare(const firmware_version_t *a, const firmware_version_t *b)`; and `bool firmware_identity_matches(const firmware_identity_t *a, const firmware_identity_t *b)`.

Define `firmware_identity_t` as a fixed 112-byte record: two little-endian `uint32_t` fields (`magic=0x31533150`, `format=1`), then `product[24]`, `hardware[24]`, `layout[24]`, `version[32]`. Identity values are `p1s-heat-controller`, `esp32-wroom-32`, `p1s-4m-ota-v1`. Versions normalize to numeric `major.minor.patch`, maximum 31 bytes; GitHub tags may have one leading `v`. Reject prerelease/build suffixes, leading-zero components, numeric overflow, and other malformed text.

Also define the types later tasks consume: `firmware_manifest_t` (identity, `filename[64]`, `uint32_t size`, raw `sha256[32]`); `firmware_attempt_t` (format 1, attempt ID, previous/candidate versions, outcome/error enums); `firmware_status_t` (installed/candidate version, phase/result, received/total bytes, maintenance/trial flags); and `firmware_check_result_t` with `FIRMWARE_CHECK_AVAILABLE`, `FIRMWARE_CHECK_UP_TO_DATE`, `FIRMWARE_CHECK_NO_RELEASE`. Phase names are `idle`, `checking`, `cooling`, `downloading`, `verifying`, `rebooting`, `trial`, `succeeded`, `failed`; `up_to_date` is a result, not installation success. Give enums stable explicit values and do not persist raw compiler-layout structs.

- [ ] **1. Write failing host tests** named `test_stable_version_order`, `test_rejects_invalid_versions`, and `test_identity_requires_exact_hardware_and_layout`. Assert `v1.0.10 > 1.0.9`, `v1.0.0 == 1.0.0`; reject `1.0`, `1.0.01`, `1.0.0-rc.1`, `4294967296.0.0`, and unterminated fixed fields. Identity mismatch must reject even when both boards use ESP32.
- [ ] **2. Run the host configure/build/test commands** and confirm the new tests fail for the expected missing policy/behavior, rather than a toolchain setup error.
- [ ] **3. Implement those interfaces and the build foundation.** Embed `custom_app_desc` in `.rodata_custom_desc`, with a size assertion and `-u custom_app_desc` linker retention. The installed SDK documents its offset as image header + first segment header + `esp_app_desc_t`; use SDK types, not a guessed offset. Add `firmware_update` to `main/CMakeLists.txt` dependencies so the identity is included in the minimal build; initially register only the policy/identity sources that exist. Set initial `version.txt` to `1.0.0` and embed that version consistently.
- [ ] **4. Configure the spec's two-slot production layout and an independent unit layout.** Unit layout keeps `device_cfg` at `0x10000`/`0x6000` and a factory test application at `0x20000`/`0x3E0000`; native OTA writes are injected in those tests. Enable production rollback and `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, `CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME`, `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`. Disable build-time private-key signing and hardware secure boot; `CONFIG_SECURE_BOOT_VERIFICATION_KEY` points to `config/ota_signing_public.bin`. The SDK requires an extracted raw binary public key, not PEM. During implementation, create or reuse an owner-controlled ECDSA key outside this checkout; never overwrite an existing key, and retain an owner backup. Commit only its extracted public key.
- [ ] **5. Make generic builds the safe default.** Root CMake reads `sdkconfig.secret` only with explicit `-DP1S_BOOTSTRAP_CREDENTIALS=ON`; release builds use OFF and a separate SDKCONFIG/build directory. Ignore local bootstrap/release build/config outputs and distribution artifacts. Rerun host tests, build unit and application images, and inspect generated configuration/partition output: two production slots fit 4 MB, bootloader fits before `0x8000`, application fits 2,031,616 bytes, hardware secure boot remains off. Stop and resolve any required layout revision before proceeding.
- [ ] **6. Commit the reviewed foundation only:** `feat: define signed OTA identity and flash layout`.

### Task 2: Persist Wi-Fi independently of release firmware

**Files:** new `device_config` component; Wi-Fi manager/header/dependencies; startup initialization/header/tests; new `test_device_config.c`; existing Wi-Fi tests; unit registration.

**Interfaces:** Produce `device_wifi_credentials_t` (`ssid[33]`, `password[64]`); `esp_err_t device_config_init(void)`; `esp_err_t device_config_load_wifi(device_wifi_credentials_t *out)`; `esp_err_t device_config_seed_wifi(const device_wifi_credentials_t *seed)`; `esp_err_t device_config_load_attempt(firmware_attempt_t *out)`; `esp_err_t device_config_save_attempt(const firmware_attempt_t *attempt)`. Keep existing Wi-Fi validation signatures; copy or alias credential fields without a component cycle. An absent record returns `ESP_ERR_NOT_FOUND`; corrupt/unknown formats return an error without erasing data. Seeding an existing valid record returns OK unchanged.

- [ ] **1. Add `[device_config]` and Wi-Fi tests:** `saved_wifi_wins_over_bootstrap`, `empty_release_reuses_saved_wifi`, `bad_or_future_config_is_not_erased`, and `attempt_record_does_not_change_wifi_or_settings`. Use test credentials only. Assert saved credentials survive named-partition deinit/reinit and an empty compiled seed; assert target `55.5` and calibration `-1.25` remain intact. Inject init/commit failure and inspect unchanged keys.
- [ ] **2. Build/run the unit suite** to establish the expected failures before implementing persistence.
- [ ] **3. Implement named-partition NVS storage and load-before-seed startup.** Store validated versioned records with a commit boundary; preserve unknown/newer keys. No automatic erase of `device_cfg`. Initialize storage after board outputs are safe and before Wi-Fi; propagate unsafe configuration errors into startup fault/trial readiness. Existing `nvs` recovery must never target the new partition. Leave shared settings schema at 1 and preserve rollback-readable records.
- [ ] **4. Run the unit suite again**, including existing settings/Wi-Fi/startup tests. Verify a generic configuration contains empty Wi-Fi defaults without displaying any personal credentials. Confirm a malformed stored record cannot fall through to destructive reseeding.
- [ ] **5. Commit:** `feat: preserve device Wi-Fi across firmware releases`.

### Task 3: Add a runtime-owned maintenance interlock

**Files:** runtime public header/implementation/output policy; existing runtime tests and host output-policy tests; API error mapping seam only if needed.

**Interfaces:** Produce `esp_err_t app_runtime_set_boot_maintenance(bool enabled)` (only before runtime start); `esp_err_t app_runtime_begin_maintenance(uint32_t timeout_ms)` (worker waits for both outputs safe); `esp_err_t app_runtime_end_maintenance(void)` (requires safe outputs and leaves heating disabled); `bool app_runtime_maintenance_active(void)`. Rejected mutating commands return existing SDK `ESP_ERR_NOT_ALLOWED`, later mapped to HTTP 409. Begin timeout is 15,000 ms; heater-off acknowledgement is required within 2,000 ms and the disable cooldown is 10,000 ms. A failed begin retains the inhibit until safe cleanup; it is not permission to flash.

- [ ] **1. Add failing tests** `maintenance_blocks_queued_enable`, `maintenance_serializes_target_save`, `maintenance_clears_manual_fan_after_cooldown`, `maintenance_timeout_never_acknowledges_safe`, and `maintenance_exit_does_not_resume_heat`. Use the existing injected sensor/output/save operations. Assert heater-off acknowledgement follows a successful driver write, fan is on before 10,000 ms and off at the deadline, enable/manual-on/target/fault-ack return `ESP_ERR_NOT_ALLOWED`, and Disable remains accepted. Coordinate concurrent tasks with semaphores rather than hoping sleeps expose a race.
- [ ] **2. Run host output-policy tests and the unit suite** to observe the failing boundary assertions.
- [ ] **3. Implement an atomic gate across command admission, target persistence, and the control task's final output choice.** Recheck queued commands when consuming them; a successful prior enqueue does not bypass the gate. Keep sensor/fault tasks running. Maintenance start disables the controller, allows the existing cooldown, then clears the manual fan override and acknowledges safe outputs. `app_runtime_stop()` alone is never the update safety mechanism.
- [ ] **4. Rerun runtime/output-policy and unit tests.** Include driver failure and full command queue, boot-maintenance mode before any task can enable, and failed cleanup retaining inhibit. A target rejected by maintenance must not invoke the save callback. Ordinary manual fan, cooldown, and offline regulation tests must continue passing.
- [ ] **5. Commit:** `feat: interlock heater outputs during firmware maintenance`.

### Task 4: Validate GitHub releases and stream authenticated images

**Files:** `firmware_manifest.{c,h}`, `firmware_transport.{c,h}`, firmware component dependencies/Kconfig; new manifest/transport unit tests and registration.

**Interfaces:** Produce `esp_err_t firmware_release_parse(const char *json, size_t length, firmware_release_t *out)` and `esp_err_t firmware_manifest_parse(const char *json, size_t length, firmware_manifest_t *out)`. Internal `firmware_release_t` contains normalized version and exactly one manifest/image asset URL, each at most 2,048 bytes. Define `firmware_candidate_t` as release + validated manifest.

Produce `esp_err_t firmware_transport_check(const char *installed, firmware_candidate_t *out, firmware_check_result_t *out_result)`; a successful check returns OK and distinguishes available/up-to-date/no-release through `out_result`, populating the candidate only when available. Transport/validation failures return errors. Produce `esp_err_t firmware_transport_stage(const firmware_candidate_t *candidate, firmware_progress_cb_t progress, void *ctx, firmware_staged_t *out)` where `firmware_progress_cb_t` is `void (*)(uint32_t received, uint32_t total, void *ctx)` and `firmware_staged_t` carries a verified inactive partition pointer. Produce `esp_err_t firmware_transport_activate(const firmware_staged_t *staged)`; stage never selects a boot partition. Put test-injectable HTTP/clock/hash/native-OTA operations in the internal transport header, with production defaults and no HTTP-server dependency.

- [ ] **1. Add failing parser tests** for one stable release, tag/manifest/image-version mismatch, duplicate/missing asset, oversized JSON, duplicate manifest keys, unknown format, bad hex digest, slot overflow, wrong product/layout, release HTML/error bodies, and invalid numeric types. Bound release JSON to 32 KiB and manifest to 2 KiB; manifest keys are `format`, `product`, `hardware`, `layout`, `version`, `filename`, `size`, `sha256`; filename is exactly `p1s-heat-controller-esp32-4mb.bin`.
- [ ] **2. Add failing injected transport tests** for clock timeout without TLS bypass, 403/429/404/5xx, five allowed HTTPS redirects versus a sixth or non-HTTPS redirect, chunked/unknown-length complete download, truncated/extra data, signature/digest/identity mismatch, allocation/flash errors, and resource cleanup. Assert no Authorization header and no boot-selection call from any stage result. Use signed fixtures for trusted compatibility identity rather than trusting manifest text.
- [ ] **3. Run the unit suite** and identify expected failing assertions.
- [ ] **4. Implement bounded HTTPS parsing and native OTA staging.** Preflight permits a 30-second clock-sync wait, 10-second socket timeout and 60-second overall check deadline; stream with a 4 KiB buffer and 180-second overall transfer deadline. Use certificate bundle verification. Fetch the fixed repository's latest full release, then its named manifest and application assets; do not accept URLs from a browser. Enforce bounded redirects and redact signed query strings in diagnostics. Hash the entire signed asset as received, accumulate the fixed image prefix across fragmented reads, and compare embedded identity to the manifest.
- [ ] **5. Use `esp_http_client_read` plus `esp_ota_begin/write/end`** so the worker can verify the complete file digest and signature before any `esp_ota_set_boot_partition` call. `esp_ota_end` provides native image/signature validation with the production config; activate is separate. Avoid high-level finish helpers that would select a slot before custom validation. Abort/close/free on every failure; report missing partition and signature support explicitly.
- [ ] **6. Rerun the unit suite**, then build the generic application and measure free heap/stack around TLS on target. Keep TLS/worker memory bounded and adjust stack sizes only from measured evidence without dropping certificate/signature checks. Commit: `feat: validate and stage public GitHub firmware releases`.

### Task 5: Compose the single update worker and persistent outcomes

**Files:** `firmware_update.c`, public `include/firmware_update.h`, `firmware_update_internal.h`; device attempt-store integration; new `test_firmware_update.c` and registration.

**Interfaces:** Consume Tasks 2–4. Produce `esp_err_t firmware_update_start(void)`, `esp_err_t firmware_update_request(void)`, `void firmware_update_get_status(firmware_status_t *out)`. Add internal `firmware_update_start_with_ops(const firmware_update_ops_t *ops)` and test stop/reset seam; injected ops cover check/stage/activate, maintenance begin/end, attempt load/save, monotonic time and restart. The status getter copies a bounded mutex-protected snapshot. Worker priority is 3, below sensor 5 and controller 6; only one request may be outstanding.

- [ ] **1. Add failing orchestration tests** for up-to-date/no-release/network preflight leaving regulation alone; duplicate request returning `ESP_ERR_NOT_ALLOWED`; stage requiring completed maintenance acknowledgement; validation/activation/record-store failure leaving the old slot selected and heating disabled; and a lost request response followed by another POST still owning one attempt. Assert progress never means confirmed success.
- [ ] **2. Add power-boundary tests**: attempt save fails before activate (no selection/restart); save succeeds but activation fails; reboot sees an attempt for a different installed version; cleanup cannot confirm safe outputs (inhibit remains). No persisted outcome may imply that a pending image passed its trial.
- [ ] **3. Run the unit suite**, confirming these traces fail before worker composition.
- [ ] **4. Implement checking → cooling → downloading → verifying → rebooting** using the earlier interfaces. Persist a new monotonically identified attempt with previous/candidate versions before activation. On preflight failure leave the runtime untouched. After maintenance begins, every failure leaves heating disabled and only releases the gate after explicit safe cleanup. Keep snapshots small; allocate candidate/URL data off the HTTP/control stacks and free them after completion. Never automatically retry a failed version or restart a job after boot.
- [ ] **5. Run the unit suite** with deterministic injected traces and confirm no overlap, correct deadlines, progress/result separation, and no deadlock between the runtime, status, and worker locks. Commit: `feat: orchestrate one-click firmware installation`.

### Task 6: Supervise trial boot and confirm or roll back

**Files:** `firmware_trial.{c,h}`, updater public/internal API; startup ops/order and main composition/dependencies; new trial tests and existing startup/runtime tests.

**Interfaces:** Produce `esp_err_t firmware_trial_start(void)` (after relay initialization, before blocking NVS/network startup); `bool firmware_trial_pending(void)`; internal `firmware_trial_start_with_ops(const firmware_trial_ops_t *ops)`. Inject running-image state, previous valid slot, clock, health snapshots, mark-valid/invalid/restart and watchdog operations. Health includes startup results, runtime freshness, sensor-pipeline freshness, saved config, Wi-Fi address and a bounded loopback GET `/api/status`.

- [ ] **1. Add failing tests** `trial_arms_before_blocking_startup`, `trial_requires_local_http_and_wifi`, `trial_does_not_require_internet_or_browser`, `physical_sensor_fault_can_remain_safely_visible`, `stale_runtime_or_trial_timeout_rolls_back`, and `confirmed_wifi_outage_does_not_rollback`. At 119,999 ms pending stays pending; at 120,000 ms it fails. Test an unexpected reset and previous-version outcome reconstruction from Task 5's record.
- [ ] **2. Add startup/first-install tests**: relays initialize before supervision; pending images set boot maintenance before runtime tasks start; fatal startup invokes rollback; the initial USB application without a previous confirmed slot remains disabled with a setup error instead of rebooting forever. Confirm that first application's health before accepting update requests. Run the unit suite to observe expected failures.
- [ ] **3. Implement the supervisor outside the unbounded service-retry loop.** Use a watchdog capable of restarting a stalled startup/health task; a timer alone is insufficient. Proposed watchdog interval is 12 seconds, fed only after bounded startup progress or fresh healthy local-task progress; normal Wi-Fi retry waiting may continue within the 120-second trial. Require sensor/status timestamps no older than the existing 2-second staleness bound. A recognized physical sensor fault is allowed only with fresh sampling, latched safe outputs and a responsive runtime.
- [ ] **4. Confirm only after all local checks pass.** Call mark-valid, then reconcile/persist result and release maintenance without enabling heat. A status-record write failure after mark-valid must report diagnostic failure, not claim the bootloader still can automatically roll back. On failed pending trials use mark-invalid-and-reboot, with watchdog fallback; never mark a sole bootstrap slot invalid. Once confirmed, stop trial-specific rollback decisions so ordinary router outages retain current behavior.
- [ ] **5. Run the unit suite and full application build.** Include startup injected failures, loopback HTTP exhaustion/retry within deadline, mark-valid/invalid errors, and the task/watchdog lifecycle after app_main returns. Commit: `feat: confirm OTA boots and recover failed trials`.

### Task 7: Expose the update API and dashboard button

**Files:** `firmware_api.{c,h}` and web-server ops/defaults/route registration/dependencies; web HTML/JS/CSS; existing API/assets tests and new dependency-free Node browser-flow test.

**Interfaces:** Extend `web_server_ops_t` with `void (*get_firmware_status)(firmware_status_t *)` and `esp_err_t (*request_firmware_update)(void)`; update every test initializer. Produce internal `esp_err_t firmware_api_register(httpd_handle_t server, const web_server_ops_t *ops)`. `GET /api/firmware` returns installed/candidate versions, phase/result, bytes, `maintenance`, `trial_pending`, `boot_id`, and `csrf_token`; the last token is a per-boot CSRF value, never a stored secret. `POST /api/firmware/update` accepts only JSON `{}` with `X-P1S-CSRF`; no URL/version/body fields may select firmware. Return 202 accepted, 409 busy/trial/maintenance conflict, 400 invalid input, 403 CSRF/origin failure, 503 unavailable. Firmware JSON has its own 1 KiB buffer and no mutation of existing status/SSE capacities.

- [ ] **1. Add failing loopback API tests** for snapshot without external network calls, accepted POST once, busy/trial 409, runtime maintenance 409, safe Disable, missing/wrong token, unexpected content type/body, foreign/null Origin, Host mismatch, legitimate IP/mDNS authorities including a nondefault test port, and OPTIONS rejection without permissive CORS. Compare exact accepted origins to Host and locally owned addresses to resist DNS rebinding; never allow an arbitrary Host just because Origin matches it.
- [ ] **2. Add Node flow tests** in `test_apps/web/test_firmware_dashboard.cjs` using built-in `node:test`, `vm` and stubbed DOM/fetch/EventSource, covering the actual `web/app.js`: one click sends one POST, heating controls follow server maintenance, lost POST response never causes an automatic retry, reboot downtime displays pending rather than success, changed boot ID refetches CSRF, changed installed version reloads assets once, and a rollback to the same version reports failure without a reload loop. These assert observable network/UI behavior, not private helper names.
- [ ] **3. Run `node --test test_apps/web/test_firmware_dashboard.cjs` and the unit suite** to establish the failures.
- [ ] **4. Implement routes and a compact Firmware card** using the existing page styling and accessible status text. Poll the firmware snapshot every 2 seconds while busy and every 15 seconds while the page is visible/idle; these local polls never query GitHub. Keep `busy` from unrelated commands separate from persistent server maintenance. Stop polling while hidden, resume with a fresh snapshot, and always let Disable remain available. Use no-store for update data and dashboard assets; browser POST failure must not reissue installation automatically.
- [ ] **5. Rerun Node and unit tests**, including four live SSE clients plus firmware status/update requests. Adjust route/socket limits within the device budget; preserve status service for trial checking. Inspect the real dashboard at desktop/mobile widths and verify the entire click/reboot/reconnect flow on the board. Commit: `feat: add firmware update control and progress to dashboard`.

### Task 8: Package releases and prove the initial installation/recovery path

**Files:** Python packaging tool/tests; explicit bootstrap/publication scripts; release guide, README and commissioning checklist; ignore entries as needed. No automatically publishing CI workflow is required.

**Interfaces:** `package_firmware.py --build-dir <dir> --public-key config/ota_signing_public.bin --output <dir> --version <x.y.z>` emits exactly the signed `.bin` asset and `firmware.json`, after native signature/size/identity/config checks. `install_bootstrap.ps1 -BuildDir <dir> -Port <COMx> -PrivateKey <outside-repo.pem>` signs the personal image and uses that build's flasher arguments with the signed application substituted. `publish_firmware.ps1 -PackageDir <dir> -Version <x.y.z>` validates and explicitly publishes assets tagged `v<version>` to the public release repository. Packaging never prints secret values or reads `sdkconfig.secret` to find them.

- [ ] **1. Add failing stdlib tests** `test_signed_asset_manifest_matches_bytes`, `test_rejects_bootstrap_or_secret_config`, `test_rejects_wrong_key_or_unsigned_image`, `test_rejects_wrong_board_version_layout_or_oversize`, and `test_publish_allowlist_excludes_source`. Use generated temporary fixture keys, synthetic config and unsigned/tampered fixtures; keep private fixture keys out of tracked files. Assert exactly the two allowlisted assets, their true signed-file SHA-256/size, strict version match, empty compiled Wi-Fi, and no bootstrap build mode. Run `python -m unittest discover -s test_apps/tools -p test_package_firmware.py -v` and confirm the intended failures.
- [ ] **2. Implement packaging and explicit scripts.** Generate or reuse the owner signing key outside the checkout and use ESP-IDF/espsecure version-1 sign/verify tooling; never regenerate the key automatically for each build. A generic build uses `idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D P1S_BOOTSTRAP_CREDENTIALS=OFF build`; a personal setup build uses a different SDKCONFIG/build directory with the flag ON. Replace unsigned application output with the verified signed asset only in the installation/package operation, not by silently publishing the whole build folder.
- [ ] **3. Prepare a precise initial USB procedure.** Record target/calibration and, if preserving them, back up/restore only the existing NVS range `0x9000`/`0x6000`. Explicitly erase the overlapping old-factory/new-`device_cfg` range `0x10000`/`0x6000` and new `otadata` range `0x16000`/`0x2000`; otherwise old executable bytes can prevent NVS initialization. Write the verified bootloader/table/signed application using the new build's checked offsets. Do not restore an old full-flash image over the new layout. Confirm stored Wi-Fi and initial application health before enclosure; generic releases thereafter use those credentials. Treat backups and the personal image as local secret-bearing artifacts.
- [ ] **4. Rerun packaging tests, host tests, Node tests, on-target Unity and complete generic/bootstrap builds.** Sign/verify the generic output, check partition/image/bootloader limits, and record commands and results. No host-only run substitutes for the board recovery rehearsal. After Unity, restore the personal OTA-capable bootstrap before testing internet updates.
- [ ] **5. Prepare public distribution without changing source visibility.** Check availability/ownership of `ahmaddawod12/P1S-HeatControl-Releases`; stage installation notes and the validated release bundle. Show the concrete public contents before the first publication. Once publication is authorized, create/use that public repository and publish only its notes and the allowlisted assets; perform anonymous metadata/asset downloads to verify device access. Never push the private source history or personal bootstrap. If the proposed name is unavailable, settle the replacement with the owner and update the fixed device source before bootstrap.
- [ ] **6. Run and record unloaded-relay hardware commissioning.** Install baseline `1.0.0`, publish/install a compatible signed `1.0.1`, then `1.0.2` to exercise both slot directions. Verify target/calibration/Wi-Fi and dashboard recovery; observe heater GPIO remains inactive throughout flashing and after reboot. Interrupt Wi-Fi/power during transfer; verify old confirmed firmware boots. Use controlled test candidates to fail local trial health and stall startup; verify watchdog/rollback, restored dashboard, retained credentials and no repeated update loop. Confirm a router outage after a confirmed update leaves regulation working normally. Remove all deliberate fault injection before any normal release and never enable mains load for these checks.
- [ ] **7. Commit completed software/documentation:** `feat: package signed OTA releases and document commissioning`. Record unavailable hardware/publication actions explicitly rather than inventing a pass. Finish with a whole-change review against the spec, the relevant test/build evidence and measured limitations before integrating or publishing a normal release.

## Self-review and handoff

Coverage: Task 1 owns flash/identity/trust foundations; Task 2 credential/settings continuity; Task 3 output safety; Task 4 bounded HTTPS and cryptographic validation; Task 5 update state/outcomes; Task 6 finite trial/recovery; Task 7 user interaction/CSRF/local status; Task 8 release distribution and real recovery evidence. The five Review Focus conditions each have named behavioral tests in their owning tasks. All task interfaces above use the shared types defined in Task 1.

Implementation has not started. This plan does not claim any new firmware build, test, board flash, repository creation, or publication has succeeded. The preferred execution method is still for the owner to choose. Author recommendation: **Native execution in this session, followed by an independent whole-change review**, because the eight tasks share runtime/storage/startup interfaces and can be implemented coherently with explicit tests. Subagent-driven execution remains available if the owner prefers independent implementer/reviewer gates for every task.
