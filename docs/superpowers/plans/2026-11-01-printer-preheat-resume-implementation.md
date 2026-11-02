# P1S Preheat and Resume Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the PC prepare a deliberately paused P1S print, then let the ESP32 preheat the chamber, send one Resume attempt, maintain temperature, and disable chamber heating when that run ends.

**Architecture:** Keep the existing thermal controller and relay ownership in `app_runtime`. Add a bounded printer-status/transport component and a separate testable cycle policy with one coordinating worker. The web server exposes printer setup and explicit per-job arming; print files and print settings stay on the PC/printer.

**Tech Stack:** C, ESP-IDF 6.1 for classic ESP32/4 MB, ESP-MQTT (`espressif/mqtt` pinned to 1.1.0), verified TLS, NVS, existing HTTP server, vanilla JS/CSS, CMake/CTest, Unity, Node's test runner.

**Spec:** [P1S status, chamber preheat, and print resume](../specs/2026-11-01-printer-preheat-resume-design.md). Read the complete spec before implementing; it owns behavior if this plan omits a detail.

Execution uses **Native** in the existing **main** checkout, as requested by the owner. Tasks 1–9 are implemented and source-tested. Task 10 remains pending physical qualification. Task 11 source verification, independent review and regression-tested fixes are complete; its native execution and physical commissioning Steps 2–3 remain pending. The original task checklists below retain the acceptance criteria, including physical steps that have not run. Actual checks and remaining work are recorded in [printer qualification](../../printer-integration-qualification.md). No flashing, live printer commands, mains commissioning, or firmware publication is authorized by this source work.

## Global Constraints

- "The owner explicitly does not want file selection or print settings on the ESP32."
- "Use the existing ESP-IDF 6.1 C architecture, classic ESP32, 4 MB flash, native HTTP server, and embedded vanilla JS/CSS."
- "`app_runtime` remains the only owner of relay hardware."
- "Preserve the DS18B20 sampling, median filtering, calibration, 0-80 degree C target range, 82 degree C raw/filtered shutdown, minimum restart interval, and fan ordering."
- "The default telemetry timeout is 30 seconds."
- "rate-limit them to at most one per 300 seconds, including reconnect storms."
- "The proposed initial maximum is 32 KiB per report, subject to measured ESP32 heap use."
- "Its proposed lifetime is 60 seconds." This refers to the one-use arm token.
- "reach the captured target at least once, then remain within target +/-1 degree C for 30 continuous seconds."
- "45 minutes from arming, including settling." This is the preheat deadline.
- "Bound the live send attempt to at most one second."
- "Do not retry an uncertain Resume."
- "maximum five-second validity" for the heat lease; the independent controller period stays 100 ms and sensor freshness stays two seconds.
- "After release, a later paused state ends chamber automation and disables heat; it is never automatically resumed."
- "Normal stop/completion uses the current ten-second disable cooldown."
- "No enabled state, lease, arm token, Resume intent, or active cycle survives a reboot."
- "Bound JSON bodies to 1,024 bytes"; printer responses initially fit 2,048 bytes. Keep the existing 512-byte `/api/status` and SSE contracts and four SSE clients.
- "Do not change the partition table, thermal settings schema, or existing Wi-Fi record." Each OTA slot is `0x1F0000` bytes.

## Review Focus

1. Reprinting the same filename with local task IDs of `0` must not reuse an earlier run's permission. Test in Tasks 1, 4, and 6.
2. A large report, escaped/Unicode filename, duplicate decision key, or queue overflow must never become a truncated valid permission to heat/resume. Test in Tasks 1, 5, and 8.
3. Two browsers, a lost HTTP response, and a repeated click must still produce at most one cycle and one Resume attempt. Test in Tasks 6, 8, and 9.
4. A stalled worker/socket or a wall-clock jump must not prolong an automatic heat lease. Test in Tasks 3, 5, and 6 using monotonic deadlines.
5. Progress reaching 100%, a subsequent filament-runout pause, or a manual resume must not be confused with successful completion or a new automatic permission. Test in Tasks 4, 6, and 9.

## Repository and file map

Work from the suitable current checkout; inspect its status and applicable instructions before execution. Create isolation only if needed at execution time. Preserve unrelated edits and stage only the files belonging to the current task. Do not reuse historical checked-off tasks or claimed test results as evidence for this feature.

| Files to create | Responsibility |
| --- | --- |
| `components/app_types/include/printer_types.h` | Bounded configuration, observation, request, cycle, and public-status types; no secrets in public status |
| `components/printer_link/{CMakeLists.txt,idf_component.yml,printer_report.c,printer_report.h,printer_status.c,printer_status.h,printer_transport.c,printer_transport.h,printer_link.c,include/printer_link.h,bambu_ca.pem}` | Strict report projection/reassembly, delta merge and run association, verified MQTT connection, status snapshot requests, and bounded live dispatch |
| `components/runtime/{runtime_auto_lease.c,runtime_auto_lease.h}` | Pure monotonic lease lifecycle; runtime integration continues to own actual heating |
| `components/clock_service/{CMakeLists.txt,clock_service.c,include/clock_service.h}` | Shared cancellable SNTP/time setup used by printer TLS and OTA |
| `components/printer_control/{CMakeLists.txt,Kconfig.projbuild,printer_cycle.c,printer_cycle.h,printer_control.c,include/printer_control.h}` | Pure cycle/token policy, qualification opt-in, and coordinating worker with injected external operations |
| `components/web_server/{printer_api.c,printer_api.h,printer_api_policy.c,printer_api_policy.h}` | Five printer routes, strict request parsing, bounded redacted response serialization |
| `web/printer.js` | Printer setup, polling, explicit arming/stop, and uncertainty feedback |
| `test_apps/host/test_printer_{report,status,link,cycle,control,api_policy}.c`, `test_apps/host/test_runtime_auto_lease.c`, `test_apps/host/test_clock_service.c` | Pure logic and real adapters with injected boundaries |
| `test_apps/host/printer_stubs/` | Only the ESP-MQTT, SNTP, and event SDK surfaces actually used by host adapter tests |
| `test_apps/unit/main/test_printer_{api,control}.c` | Native HTTP/runtime/lifecycle integration with fake printer transport |
| `test_apps/web/test_printer_dashboard.cjs` | Browser behavior using the existing VM/fake-DOM approach |
| `test_apps/fixtures/printer/README.md`, `docs/printer-integration-qualification.md`, `docs/printer-startup-profile.md` | Sanitized protocol fixtures, measured compatibility results, and exact qualified PC-profile instructions |

Modify existing files only in their owning task: `device_config.c` and its header; `app_runtime.c` and its header/CMake; service-operation header/implementation; firmware transport/update and CMake; startup implementation/header/CMake; web-server implementation/header/CMake; embedded web assets and `app.js`; host/unit CMake and relevant existing tests; README and commissioning guidance. Existing components and the pure heat state machine retain their responsibilities.

## Shared contracts and execution conventions

Types in `printer_types.h` are the common vocabulary. Define the named types here in Task 1; later tasks extend their implementation, not their meaning:

- `printer_connection_config_t`: `bool enabled`, `ipv4[16]`, `serial[33]`, `access_code[65]`, `ntp_host[254]`.
- `printer_run_state_t`: unknown, idle, preparing, running, paused, finished, failed. Map wire values only through the report module; unknown is the zero initializer.
- `printer_report_delta_t`: a field-presence mask and decoded state, layer count, run-start identifier, task/subtask IDs, file, label, print type, progress, remaining time, temperatures, and errors. Omitted fields are absent; explicit invalid decision fields invalidate the report. Bound file to 256 UTF-8 bytes, label to 96, IDs to 64, and print type to 32; overlong identity cannot authorize automation.
- `printer_job_key_t`: connection generation, observed print epoch, available identity-field mask/values. Nonzero run start plus identifying metadata, or continuously observed new-job history, establishes a run; a filename or `0` ID alone never does.
- `printer_observation_t`: `state`, `job`, `connection_epoch`, `received_ms`, validity flags, `baseline_valid`, `startup_eligible`, `blocking_error`, progress/remaining-time validity and values, temperatures, and bounded diagnostic reason. `startup_eligible` is evidence of a candidate initial pause, not operator authorization.
- `printer_arm_request_t`: `arm_token[33]` and `bool initial_pause_confirmed`; token is 16 random bytes encoded as 32 hex characters.
- `printer_config_request_t`: complete `printer_connection_config_t value` plus `bool access_code_present` to distinguish omission from replacement.
- `printer_cycle_state_t`: ready, preheating, resume_pending, printing, completed, stopped, error. Define `printer_cycle_outcome_t` here for completed, operator_stop, later_pause, cancelled, printer_error, identity_changed, telemetry_stale, link_lost, heat_fault, lease_expired, preheat_timeout, resume_rejected, and resume_uncertain; include none as its zero initializer. `printer_cycle_snapshot_t` holds cycle ID/state/outcome, captured target/job/generation, deadlines, Resume-attempted flag, and offered token/expiry.
- `printer_dispatch_result_t`: `PRINTER_DISPATCH_REJECTED`, `PRINTER_DISPATCH_SENT`, `PRINTER_DISPATCH_UNKNOWN`. This shared type belongs in `app_types`, so the link component never depends on the control component.
- `printer_control_status_t`: redacted connection configuration (`access_code_set` only), observation, cycle snapshot, `can_arm`, eligibility reason, and configuration-operation result. Never embed `printer_connection_config_t` directly in a public status object.

All policy time arguments are monotonic `uint64_t now_ms`. Wall time is used only for TLS certificate validity. Keep UTF-8 string lengths and JSON escaped output lengths separate.

Add `CONFIG_P1S_PRINTER_AUTO_RESUME` in the Task 6 Kconfig file, default `n`. This is the plan's concrete enforcement of the spec's qualification gate, not a print option: monitoring/configuration remain usable, but production arming reports `qualification_required` while it is off. Only an explicitly authorized commissioning build may opt in while Task 10 validates the workflow. A normal build must not enable it merely because a connection succeeds or the user confirms an arbitrary pause.

Commands below run from `C:\Users\dawud\P1S-HeatControl` in an activated ESP-IDF 6.1 PowerShell environment. A new host CTest target must be added to `test_apps/host/CMakeLists.txt` before its red run; keep the current `-Wall -Wextra -Werror -pedantic` checks. Reconfigure when adding targets:

```powershell
cmake -S test_apps/host -B test_apps/host/build -G 'MinGW Makefiles'
cmake --build test_apps/host/build
ctest --test-dir test_apps/host/build --output-on-failure -R '<task regex>'
```

For each task: write the named failing tests, run them and record the actual failure, implement the interfaces, rerun the same target, then commit only that task's files. A missing new interface/build target is an acceptable first red result for a new module; subsequent behavior changes need an assertion failure. Expected results below are future verification criteria, not results already observed.

---

### Task 1: Bounded printer reports and run observations

**Files:** Create `printer_types.h`, `printer_link/printer_report.{c,h}`, `printer_link/printer_status.{c,h}`, initial component CMake, host `test_printer_report.c`, `test_printer_status.c`, and fixture README. Modify host CMake.

**Interfaces:** Produce:

```c
esp_err_t printer_report_parse(const char *json, size_t len, printer_report_delta_t *out);
printer_frame_result_t printer_frame_feed(printer_frame_t *frame, const printer_fragment_t *part,
                                         const char **json, size_t *len);
void printer_status_init(printer_status_t *status, uint32_t connection_epoch);
bool printer_status_apply(printer_status_t *status, const printer_report_delta_t *delta,
                         uint32_t connection_epoch, uint64_t now_ms);
void printer_status_snapshot(const printer_status_t *status, uint64_t now_ms,
                             printer_observation_t *out);
bool printer_job_same(const printer_job_key_t *a, const printer_job_key_t *b);
```

`printer_fragment_t` carries topic+length, bytes+length, offset/total length, generation, and retained flag; frame results are pending/complete/rejected. `printer_frame_t` owns a 32,768-byte maximum frame plus terminator. `printer_status_t` owns merged values and connection-local new-job history.

- [ ] **Step 1 — Write the failing tests.** Add `report_requires_complete_valid_json`, `fragment_offsets_are_checked`, `retained_and_wrong_generation_do_not_refresh`, `delta_omission_preserves_state`, `zero_ids_require_observed_new_run`, and `same_filename_is_not_same_run`. Include these assertions with local values initialized in each test:

  ```c
  assert(printer_report_parse(valid_json, sizeof(valid_json) - 1, &delta) == ESP_OK);
  assert(printer_report_parse(duplicate_state_json, sizeof(duplicate_state_json) - 1, &delta) != ESP_OK);
  assert(observation.state == PRINTER_RUN_PAUSED); /* preserved by a progress-only delta */
  assert(!observation.startup_eligible);           /* first observation is an ambiguous pause */
  assert(!printer_job_same(&previous_run, &next_run)); /* identical filename, new run evidence */
  ```

  Also cover numeric/string forms actually documented for fields, invalid UTF-8/escapes and trailing data, depth/size exhaustion, malformed ignored subtrees, duplicate nested error/identity fields, absent versus zero progress, stale telemetry at 29,999/30,000 ms, and unknown states. Use explicitly synthetic fixtures until real sanitized captures exist; do not present them as measured P1S traffic.
- [ ] **Step 2 — Run the red check.** Run the common host commands with regex `printer_(report|status)`. Expected: nonzero for the missing module or the first named behavioral assertion.
- [ ] **Step 3 — Implement projection and merge.** Use a bounded, strict token projection into caller-owned data instead of constructing an unrestricted cJSON tree from a 32 KiB printer message. Fix a parser depth limit of 16 and reject resource exhaustion. Validate the entire JSON before committing any delta. Preserve exact decoded identity bytes. Only recognized live status reports refresh telemetry; unrelated replies do not. Reset all baseline/history on generation change and implement the spec's strong-identity/history alternatives.
- [ ] **Step 4 — Run the green check.** Repeat the same host commands. Expected: both CTest targets pass, including oversize/malformed cases with no valid observation produced.
- [ ] **Step 5 — Commit.** Commit this task's explicit files with message `feat: project and merge bounded P1S status reports`.

### Task 2: Optional printer configuration in device NVS

**Files:** Modify `components/device_config/{device_config.c,include/device_config.h}`, `test_apps/host/{test_device_config.c,stubs/nvs.h}`, and `test_apps/unit/main/test_device_config.c`.

**Interfaces:** Consume `printer_connection_config_t`; produce:

```c
bool printer_config_valid(const printer_connection_config_t *config);
esp_err_t device_config_load_printer(printer_connection_config_t *out);
esp_err_t device_config_save_printer(const printer_connection_config_t *config);
esp_err_t device_config_clear_printer(void);
```

- [ ] **Step 1 — Write the failing tests.** Extend `test_device_config` with missing-record, exact round-trip, bad/future-record, failed commit, explicit clear, and partition-failure cases. Increase the fake record buffer to at least 376 bytes and its record capacity to six; implement `nvs_erase_key` in the stub. Assert:

  ```c
  assert(device_config_load_printer(&loaded) == ESP_ERR_NOT_FOUND);
  assert(device_config_save_printer(&saved) == ESP_OK);
  assert(strcmp(loaded.access_code, saved.access_code) == 0); /* after successful reload */
  assert(device_config_clear_printer() == ESP_OK);
  assert(device_config_clear_printer() == ESP_OK); /* idempotent */
  ```

  Compare Wi-Fi, OTA-attempt, and bootstrap-confirmation records before/after save, failed commit, and clear. Assert malformed/future printer data is not silently overwritten by a save and is not treated as empty configuration. An explicit clear may remove only this optional record.
- [ ] **Step 2 — Run red.** Run host configure/build and CTest regex `device_config`. Expected: the new API is missing or a new assertion fails.
- [ ] **Step 3 — Implement the codec and validation.** Add `printer_v1` in the existing `p1s_device` namespace of `device_cfg`, using explicit serialization: version u32 at 0, enabled u32 at 4, IPv4 bytes at 8, serial at 24, code at 57, NTP host at 122; total 376 bytes. Validate terminators, flags, IPv4, ASCII serial, exact code bytes/no NUL, and hostname/IP limits. Reuse existing atomic commit behavior; do not store raw C struct padding or seed printer secrets from build files. Missing/corrupt optional data and an actual partition initialization failure remain distinguishable.
- [ ] **Step 4 — Run green.** Repeat the host target and build the native unit image without flashing. Expected: existing device records remain compatible and all new cases pass.
- [ ] **Step 5 — Commit.** Message: `feat: persist optional printer connection settings`.

### Task 3: Independent automatic-heating lease and command ownership

**Files:** Create `components/runtime/runtime_auto_lease.{c,h}` and `test_apps/host/test_runtime_auto_lease.c`. Modify runtime CMake, `app_runtime.c`, `include/app_runtime.h`, `include/service_operation.h`, `service_operation.c`, host CMake, `test_runtime_maintenance.c`, `test_service_operation.c`, and native `test_runtime.c`.

**Interfaces:** Define `runtime_auto_lease_t` with active ID/expiry and a consumed-ID high-water mark, and `runtime_auto_end_reason_t` with none, operator_stop, expired, fault, and maintenance. Produce pure `runtime_auto_lease_init(runtime_auto_lease_t *) -> void`, `runtime_auto_lease_acquire/renew(runtime_auto_lease_t *, uint64_t cycle_id, uint64_t now_ms, uint64_t expires_ms) -> esp_err_t`, `runtime_auto_lease_revoke(runtime_auto_lease_t *, uint64_t cycle_id, runtime_auto_end_reason_t) -> void`, and `runtime_auto_lease_step(runtime_auto_lease_t *, uint64_t now_ms) -> bool`. Export:

```c
esp_err_t app_runtime_auto_acquire(uint64_t cycle_id, float target_c, uint64_t expires_ms);
esp_err_t app_runtime_auto_renew(uint64_t cycle_id, uint64_t expires_ms);
void app_runtime_auto_revoke(uint64_t cycle_id);
void app_runtime_get_auto_status(runtime_auto_status_t *out);
```

`runtime_auto_status_t` records active ID, expiry, last-ended ID/reason, and whether controller teardown has been applied. IDs are increasing within a boot; zero is invalid for acquire/renew and means revoke the current automatic owner for the revoke function.

- [ ] **Step 1 — Write failing lease/runtime tests.** Assert exactly 5,000 ms is the maximum grant, expiry is inclusive, an expired/revoked ID cannot be reacquired or renewed, a newer ID can be admitted after teardown, and renewing does not re-enable a consumed owner. Test a real host runtime worker with the automation producer stopped and with the ordinary command queue saturated; heater disable must still occur on expiry. Check these results:

  ```c
  assert(app_runtime_auto_renew(expired_id, new_deadline) == ESP_ERR_INVALID_STATE);
  assert(app_runtime_set_target(55.0f) == ESP_ERR_NOT_ALLOWED); /* active auto owner */
  assert(app_runtime_set_enabled(true) == ESP_ERR_NOT_ALLOWED);
  assert(app_runtime_set_enabled(false) == ESP_OK);           /* always available */
  assert(!auto_status.active && auto_status.teardown_applied);
  ```

  Add tests for sensor fault, 82 degree C trip, maintenance/boot inhibition, target-zero rejection, manual-fan override, ten-second normal cooldown, and immediate fault shutdown. Test wall-time changes separately from monotonic expiry. Test all service-owner pairs after adding `SERVICE_OPERATION_PRINTER_CONFIG` and `SERVICE_OPERATION_PRINTER_CYCLE`.
- [ ] **Step 2 — Run red.** Use regex `runtime_(auto_lease|maintenance|output_policy)|service_operation`. Expected: missing API or failed lease/ownership assertion.
- [ ] **Step 3 — Implement lease enforcement.** Keep the pure controller unchanged. Add short protected runtime ownership state and an urgent revoke/disable path independent of the ordinary queue. The 100 ms controller checks expiry and faults every iteration, invalidates consumed IDs, and publishes teardown acknowledgement. Recheck ownership in target persistence/admission paths so a pre-existing queued manual command cannot change the captured target. No network work enters this component. Update service-operation enum bounds and leave the gate itself independent of heater control.
- [ ] **Step 4 — Run green.** Repeat the targeted host tests and build native unit tests. Expected: expiry shuts down heating without help from the printer worker; existing manual/maintenance tests stay green.
- [ ] **Step 5 — Commit.** Message: `feat: enforce revocable automatic heat ownership`.

### Task 4: Pure preheat, arm-token, and one-attempt cycle policy

**Files:** Create `components/printer_control/{printer_cycle.c,printer_cycle.h,CMakeLists.txt}` and `test_apps/host/test_printer_cycle.c`; modify host CMake.

**Interfaces:** Consume Tasks 1/3 observations and runtime status. Define `printer_cycle_input_t` containing observation, `app_status_t` heat status, `runtime_auto_status_t`, monotonic time, and maintenance/qualification eligibility. Produce:

```c
void printer_cycle_init(printer_cycle_t *cycle);
void printer_cycle_offer_arm(printer_cycle_t *cycle, const printer_cycle_input_t *in,
                             const char fresh_token[33]);
esp_err_t printer_cycle_arm(printer_cycle_t *cycle, const printer_arm_request_t *request,
                           const printer_cycle_input_t *in, uint64_t cycle_id);
void printer_cycle_step(printer_cycle_t *cycle, const printer_cycle_input_t *in,
                        printer_cycle_effects_t *out);
void printer_cycle_note_dispatch(printer_cycle_t *cycle, printer_dispatch_result_t result,
                                uint64_t now_ms);
void printer_cycle_stop(printer_cycle_t *cycle, printer_cycle_outcome_t reason, uint64_t now_ms);
void printer_cycle_snapshot(const printer_cycle_t *cycle, printer_cycle_snapshot_t *out);
```

Effects are `renew_heat`, `revoke_heat`, and `send_resume`. Dispatch results distinguish rejected-before-send, sent, and unknown. No function performs I/O or drives hardware.

- [ ] **Step 1 — Write failing policy tests.** Use deterministic timestamps and fresh sensor/report timestamps at each simulated tick. Cover no implicit arm, 60,000 ms token expiry, stable token across harmless deltas, immediate token invalidation on relevant changes, explicit confirmation, and duplicate-token rejection. For target 60, reach 60 once and hold 59-61 for 30,000 continuous ms: assert no Resume at 29,999 and one at 30,000; crossing below 59 or above 61 resets settling. An already warm chamber still settles. Assert no Resume before ever reaching 60. Also assert:

  ```c
  assert(effects.send_resume);  /* first qualified readiness transition only */
  assert(snapshot.resume_attempted);
  assert(!effects.send_resume); /* subsequent tick, duplicate reply, or reconnect */
  assert(snapshot.state == PRINTER_CYCLE_ERROR); /* 2,700,000 ms preheat timeout */
  ```

  Test freshness at exactly 30,000 ms and sensor age at the existing boundary; 30,000 ms dispatch-confirmation deadline; matching terminal status before observed running; 100% progress while still running; every fault/cancel/new-job/idle path; a later pause followed by manual resume; and local stop. An unknown send result must never authorize another attempt.
- [ ] **Step 2 — Run red.** Use CTest regex `printer_cycle`. Expected: missing policy or named state/side-effect assertion failure.
- [ ] **Step 3 — Implement the state machine.** Apply the spec's precedence: inhibit/revoke events outrank readiness or progress; consume Resume permission before emitting its effect. Capture target/run/generation once. Leave terminal outcome visible while issuing a fresh token only for a newly eligible deliberate cycle. Treat ambiguous state or identity as ineligible.
- [ ] **Step 4 — Run green.** Repeat the target. Expected: all state/deadline/one-attempt tests pass without any ESP-IDF networking or real-time sleeps.
- [ ] **Step 5 — Commit.** Message: `feat: model explicit P1S preheat and resume cycles`.

### Task 5: Verified MQTT status transport and shared time setup

**Files:** Create the remaining `printer_link` files/CA/manifest, `clock_service`, host `test_printer_link.c`, `test_clock_service.c`, and minimal `printer_stubs`. Modify host CMake, `components/app_startup/CMakeLists.txt`, and `components/firmware_update/{firmware_transport.c,CMakeLists.txt}` to share time setup and compile the optional transport in the minimal build.

**Interfaces:** Produce `printer_link_start(void)`, `printer_link_start_with_ops(const printer_link_ops_t *)`, `printer_link_configure(const printer_connection_config_t *)` returning `esp_err_t`; `printer_link_tick(uint64_t now_ms)`, `printer_link_snapshot(printer_observation_t *)`, and `printer_link_suspend(bool)` return void. `tick` is called by the Task 6 worker, which is the single owner of status decisions. Event callbacks only assemble/project bounded messages and enqueue immutable deltas or invalidate a connection generation. Define `printer_link_ops_t` in `printer_link.h` with these callbacks and a final `void *ctx`:

```c
esp_err_t (*connect)(const printer_connection_config_t *config, void *ctx);
esp_err_t (*subscribe)(const char *topic, void *ctx);
printer_dispatch_result_t (*publish_live)(const char *topic, const char *json, size_t len,
    int qos, bool retain, uint64_t deadline_ms, void *ctx);
void (*abort)(void *ctx);
uint64_t (*now_ms)(void *ctx);
```

Shared time entry point:

```c
esp_err_t clock_service_wait(const char *ntp_host, uint32_t timeout_ms,
                             bool (*cancelled)(void *), void *ctx);
```

It coordinates the one SNTP service, polls cancellation at most every 100 ms, and never treats an uninitialized clock as certificate-valid.

- [ ] **Step 1 — Write failing adapter tests.** Add `test_tls_identity_and_snapshot_cooldown`, `test_overflow_invalidates_generation`, and `test_shared_clock_cancellation`, using real component code with only SDK calls replaced and a local recording fake. Pin these assertions to recorded arguments/counters:

  ```c
  assert(recorded_port == 8883);
  assert(strcmp(recorded_username, "bblp") == 0);
  assert(!recorded_skip_certificate_check && !recorded_skip_name_check);
  assert(snapshot_request_count == 1); /* reconnects before 300,000 ms */
  assert(!observation.baseline_valid); /* queue overflow or wrong generation */
  ```

  Check separate credential fields, clean session, embedded Bambu CA, and expected serial name verification. Wrong CA/name, missing time, network/auth failure, and cancellation must not produce an eligible observation. On connect, check the exact report subscription and `pushall` request; at reconnect times 1,000 and 299,999 ms forbid another snapshot request, then allow one at 300,000. Snapshot cooldown survives disconnect events. Retained data and old generations cannot refresh state. Simultaneous OTA/printer clock users must not independently initialize/deinitialize SNTP; wall-clock changes must not affect monotonic deadlines.
- [ ] **Step 2 — Run red.** Use regex `printer_link|clock_service` after adding fake SDK definitions. Expected: missing adapters or a failed configuration/timing assertion.
- [ ] **Step 3 — Implement shared time setup.** Implement `clock_service_wait` and move the existing OTA `clock_ready` behavior behind it, retaining the normal default host and deadline. Serialize SNTP lifecycle changes, support a LAN NTP host, and enforce cancellation within 100 ms without holding a runtime/status lock.
- [ ] **Step 4 — Implement status monitoring.** Pin `espressif/mqtt: "==1.1.0"`; embed the verified Bambu CA from the spec's primary protocol reference with provenance in a source comment. Add `printer_link` as a private build dependency of `app_startup` now so `MINIMAL_BUILD ON` actually compiles the new transport/client; starting the service remains Task 7. Use native Wi-Fi/IP events to invalidate generations immediately; handle an event loop not yet available through idempotent retry. Use 1/2/4/8/30-second connection backoff. Bound report storage/mailboxes; overflow invalidates the baseline rather than allowing an old decision. No access code appears in URI/log/status. This task sends status requests only, with no automatic print command.
- [ ] **Step 5 — Run green and build.** Repeat host tests, existing OTA transfer/job/trial tests, and `idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D P1S_BOOTSTRAP_CREDENTIALS=OFF build`. Expected: all targeted tests pass and ESP-IDF 6.1 resolves/builds the pinned client with verified-TLS support; no source fallback to unverified TLS. Hardware connection remains unclaimed.
- [ ] **Step 6 — Commit.** Message: `feat: monitor P1S over verified local MQTT`.

### Task 6: Coordinated arming, runtime leases, and bounded Resume dispatch

**Files:** Create `printer_control.c`, `include/printer_control.h`, `Kconfig.projbuild`, and host `test_printer_control.c`. Modify printer-control CMake, `printer_link/{printer_link.c,printer_transport.c,printer_transport.h,include/printer_link.h}`, and relevant link tests.

**Interfaces:** Consume Tasks 1-5. Produce public `printer_control_start(void)` / `printer_control_start_with_ops(const printer_control_ops_t *)`, `printer_control_request_config(const printer_config_request_t *)`, `printer_control_forget_config(void)`, `printer_control_arm(const printer_arm_request_t *)`, `printer_control_stop(void)` returning `esp_err_t`; `printer_control_get_status(printer_control_status_t *)` returns void. In `printer_control_ops_t`, use callbacks named `heat_acquire`, `heat_renew`, `heat_revoke`, `heat_snapshot`, `auto_snapshot`, `link_configure`, `link_tick`, `link_snapshot`, `link_resume`, `load_config`, `save_config`, `clear_config`, `gate_begin`, and `gate_end`, with the signatures of the corresponding Tasks 1-5 public functions plus a final `void *ctx`. Also define `uint64_t (*now_ms)(void *)`, `void (*random_bytes)(void *out, size_t len, void *ctx)`, and `bool resume_qualified`; production qualification is the Kconfig option, while fake tests may explicitly enable it. Add:

```c
printer_dispatch_result_t printer_link_resume_once(uint64_t cycle_id,
    const printer_job_key_t *expected_job, uint32_t expected_epoch, uint64_t deadline_ms);
```

- [ ] **Step 1 — Write failing orchestration tests.** Exercise the actual coordinator using fake operations. With `resume_qualified=false`, assert arming returns conflict with `qualification_required` while monitoring still works. With it explicitly true in the fake, arm two clients with the same token, lose the response, resubmit, replace the job, and disconnect during send: total publish attempts must remain at most one for the consumed cycle. Assert every Resume is QoS 0, nonretained, issued only while live, and has no filename/options. Block the fake send and require abort at the 1,000 ms deadline; waking it later must not flush a queued request. Stall the coordinator while the Task 3 runtime runs and prove lease expiry and urgent Stop without waiting for that worker. Force a failed acquire/renew/save and verify owner/gate cleanup and status results. Clear/reconfigure may not disturb the working link before persistence succeeds. Explicitly test the same filename/zero IDs for a second print, later pauses, retained error reports, and pending manual target commands.

  Name the primary cases `test_duplicate_arm_has_one_attempt`, `test_blocked_send_is_aborted`, and `test_stalled_coordinator_loses_heat_ownership`; use their local recording fakes to assert:

  ```c
  assert(resume_publish_count == 1);
  assert(resume_qos == 0 && !resume_retain);
  assert(abort_count == 1 && unsent_request_count == 0);
  assert(!auto_status.active && auto_status.teardown_applied);
  ```
- [ ] **Step 2 — Run red.** Use regex `printer_control|printer_link|runtime_(auto_lease|maintenance)`. Expected: missing coordinator or failed one-attempt/lease/ownership assertion.
- [ ] **Step 3 — Implement the bounded live dispatcher.** Implement `printer_link_resume_once`, revalidating the job/generation immediately before its one live QoS 0 call and consuming transport permission before I/O. Configure and verify the one-second end-to-end send bound, including SDK lock/write behavior; an individual socket timeout alone is insufficient evidence. If the native adapter cannot enforce it, keep production Resume unavailable and resolve the transport contract before proceeding to qualification. Do not substitute QoS 1/2 or an outbox.

- [ ] **Step 4 — Implement the coordinator and heat bridge.** Drain status events, process admissions, update policy, renew leases, and dispatch Resume through one worker; no network call runs under a runtime/status lock. Revalidate current runtime readiness before calling the dispatcher. Add the default-off qualification option and bind production `resume_qualified` to it. Acquire the cycle service owner before arming; acknowledge admission only after token/eligibility checks and local ownership acquisition. Renew at one-second intervals with expiry no later than now+5,000 ms or the remaining telemetry/deadline bound. Revoke on any disqualifier. Hold the service owner until runtime teardown is acknowledged; a stale worker must not revive a consumed ID. Stop sets an urgent consumed/stop flag and calls runtime revoke without waiting behind network work. `printer_control_get_status` overlays live runtime expiry/teardown onto its cached cycle status so a stalled worker cannot leave the dashboard claiming active heating authority. Do not let an old completion release a newer cycle's service owner.

- [ ] **Step 5 — Implement configuration admissions.** Configuration saves use asynchronous admission (202 later at HTTP); forget completes its NVS clear before reporting success. Use a bounded command/completion mailbox; an uncertain request result is visible in status and never internally retried. Obtain IDs from a boot-unique increasing sequence and tokens from `esp_fill_random`, not print names or timestamps.
- [ ] **Step 6 — Run green.** Repeat the target group. Expected: the complete fake end-to-end preheat/run/finish cycle and every failure path pass; outbound recording contains only status requests and an authorized single Resume.
- [ ] **Step 7 — Commit.** Message: `feat: coordinate one-attempt printer resume with heat leases`.

### Task 7: Optional startup, service exclusion, and OTA coexistence

**Files:** Modify `components/app_startup/{app_startup.c,include/app_startup.h,CMakeLists.txt}`, `components/firmware_update/{firmware_update.c,CMakeLists.txt}`, and printer control/link as needed. Modify native `test_app_startup.c`, `test_firmware_admission.c`, host `test_service_operation.c`, `test_wifi_manager.c`, `test_wifi_ap_shutdown.c`, and control tests.

**Interfaces:** Add optional `esp_err_t (*printer_start)(void)` to `app_startup_ops_t`; default points to `printer_control_start`. Existing required steps and their order remain unchanged. Use `printer_link_suspend(bool)` around admitted idle OTA work; printer availability never enters `firmware_trial_health_t`.

- [ ] **Step 1 — Write failing integration tests.** Assert relays/trial/storage/runtime/Wi-Fi/web ordering is preserved and printer startup is optional and idempotent. An absent printer, corrupt optional record, or failed printer-worker allocation must not prevent the dashboard or change OTA trial health. With a cycle owner active, Wi-Fi changes, printer config/clear, and OTA return conflict; read-only status and local stop still work. Verify owner release on every failed admission/teardown and exercise real Wi-Fi/AP policies with the new enum values. On idle OTA, assert monitoring resources are suspended/released before TLS/update work and monitoring-only reconnection after a failed update.

  Add `test_optional_printer_failure_is_nonfatal` and `test_cycle_excludes_maintenance` with injected failures; assert `app_startup_run(&ops) == ESP_OK` for an optional-printer-only failure and `firmware_update_request() == ESP_ERR_NOT_ALLOWED` while the cycle gate is owned. Compare the recorded required startup sequence with the existing expected sequence.
- [ ] **Step 2 — Run red.** Use regex `printer_control|service_operation|wifi_manager|wifi_ap_shutdown|firmware_(job|trial)` and build the Unity startup/admission cases. Expected: failed optional-start/gate/resource assertion.
- [ ] **Step 3 — Wire lifecycle hooks.** Start the optional service after the existing required services without making printer availability a fatal dependency. Preserve the existing single service gate; do not create a second competing gate or alter Wi-Fi credential records. Suspend idle printer transport for OTA and ensure cancellation frees a pending SNTP/connection attempt. On reboot, restore configuration only; no active heat or pending Resume is reconstructed.
- [ ] **Step 4 — Run green.** Repeat the targeted host group and native unit build. Expected: existing manual startup, fallback hotspot, maintenance, and rollback behavior remain covered and pass; optional printer failure stays local to printer status.
- [ ] **Step 5 — Commit.** Message: `feat: integrate printer lifecycle with startup and maintenance`.

### Task 8: Strict printer HTTP routes and redacted status

**Files:** Create `printer_api.{c,h}`, `printer_api_policy.{c,h}`, host `test_printer_api_policy.c`, and native `test_printer_api.c`. Modify web-server CMake/header/implementation, host/unit CMake, and existing native API tests.

**Interfaces:** Produce `esp_err_t printer_api_register(httpd_handle_t, const web_server_ops_t *)`, `bool printer_api_parse_config(const char *, size_t, printer_config_request_t *)`, `bool printer_api_parse_arm(const char *, size_t, printer_arm_request_t *)`, and `esp_err_t printer_api_status_json(const printer_control_status_t *, const char *csrf, char *out, size_t capacity)`. Extend `web_server_ops_t` with callbacks matching Task 6's public status/config/forget/arm/stop signatures; missing optional callbacks report unavailable rather than invalidating the existing server.

- [ ] **Step 1 — Write failing policy/native tests.** Test the five spec routes and exact 202/204/400/403/409/503 responses, including a lost arm reply/duplicate request. Cover body lengths 1,024 and 1,025 bytes, duplicate/unknown keys, trailing JSON, embedded NUL, wrong content type/types, missing code for a new serial, omitted code preserved for the same serial, invalid addresses, and maximum escaped job labels. Assert no secret in any GET/error response or logs; public status fits 2,048 bytes or returns an explicit error without truncation. Test Host/Origin/token validation on both STA/AP addresses. With four SSE streams open, exercise GET status, arm, and stop. Existing status remains bounded by 512 bytes.

  Add `test_printer_api_rejects_ambiguous_body`, `test_printer_status_never_contains_code`, and native `printer_routes_coexist_with_four_sse_clients`. Against locally constructed request/status fixtures assert `!printer_api_parse_arm(duplicate_json, sizeof(duplicate_json) - 1, &request)` and `strstr(response_json, secret_access_code) == NULL`; native responses must expose 409 rather than a second admission for a consumed token.
- [ ] **Step 2 — Run red.** Use regex `printer_api_policy|firmware_api_policy|wifi_api_policy`; build native `[printer_api]` cases. Expected: missing route/policy or failed HTTP/privacy assertion.
- [ ] **Step 3 — Implement policy and routes.** Bind exactly `GET /api/printer`, `PUT/DELETE /api/printer/config`, `POST /api/printer/cycle`, and `POST /api/printer/cycle/stop`. Reuse `firmware_api_request_trusted` and the per-boot token for all new mutations; preserve the existing trusted-LAN access model. Return no-store responses. Map ineligible/owned states to 409 with a printer-specific reason rather than the current generic maintenance message, including existing target/enable routes refused by automatic ownership. Increase `max_uri_handlers` from 20 to 28 to cover these routes plus the upcoming script; keep socket/SSE budgets under the configured 16 lwIP sockets. Register the native printer test files and direct `printer_control`, `printer_link`, and `clock_service` unit-component dependencies so their headers and implementations are actually compiled. Do not add printer fields to `/api/status` or SSE.
- [ ] **Step 4 — Run green.** Repeat host checks and unit build; execute native HTTP cases only on an explicitly selected authorized board. Expected: fake transport receives zero print-setting/new-print operations; all routes can coexist with the four SSE clients.
- [ ] **Step 5 — Commit.** Message: `feat: expose guarded printer setup and cycle APIs`.

### Task 9: Existing-dashboard printer controls

**Files:** Create `web/printer.js`, browser `test_printer_dashboard.cjs`. Modify `web/{index.html,styles.css,app.js,CMakeLists.txt}`, web-server embedded asset handler, existing browser tests, and native `test_web_assets.c`.

**Interfaces:** Poll Task 8's contract every 1,000 ms while visible, one in-flight GET at a time. Publish `p1s-printer-control` CustomEvents with `{active, known}` so existing controls can reflect automatic ownership without changing their API schemas. Serve `/printer.js` with the existing NUL-excluding asset-length helper.

- [ ] **Step 1 — Write failing browser/asset tests.** Follow the current `test_wifi_dashboard.cjs` VM/fake-DOM setup. Assert edited fields survive polling, the access code is never populated from GET, and omitted code preserves the same-printer secret. Confirm arming requires the current offered token and an unchecked-by-default startup confirmation; disable a second click during admission. Lose the mutation response, poll repeatedly, and assert exactly one POST, uncertainty copy, and no invented success. Test two tabs with a consumed token, hidden/visible polling, stale/out-of-order GET responses, disconnect during preheat, terminal outcome, and 100% progress while running. Assert these button labels exactly:

  ```js
  assert.equal(preheatButton.textContent, 'Preheat & Resume');
  assert.equal(stopButton.textContent, 'Stop chamber control');
  assert.equal(armPosts.length, 1); // after duplicate clicks and lost-response polling
  ```

  During a cycle, target/heat-enable/update controls lock; manual fan and local disable/Stop remain available. After a later pause/manual resume, the UI must show that automatic chamber control ended. Verify no file browser, print options, or remote frontend dependencies are introduced and the served script has no trailing embedded NUL.
- [ ] **Step 2 — Run red.** Run `node --test test_apps/web/test_printer_dashboard.cjs` and build native asset tests. Expected: missing asset/elements or the first failing browser behavior.
- [ ] **Step 3 — Implement the existing-page extension.** Reuse current typography/components and touch-sized controls. Add address/serial/code/NTP setup plus compact status/progress/readiness/outcome. Use textContent for printer-supplied text. Distinguish saving/connecting, queued/active, uncertain, and completed results. State that stopping chamber control does not cancel the print. In `app.js`, add an automatic-owner lock only to target/enable/update paths, not to the manual-fan lock; backend ownership remains authoritative during UI races. Never repeat mutations automatically.
- [ ] **Step 4 — Run green.** Run `node --test test_apps/web/*.cjs` and unit build. Expected: all current and new dashboard tests pass. On a local dashboard preview or authorized device, inspect narrow/wide layouts, labels, disabled reasons, keyboard/focus use, and unchanged manual operation.
- [ ] **Step 5 — Commit.** Message: `feat: add printer preheat and resume dashboard controls`.

### Task 10: Compatibility qualification and the PC startup profile

**Files:** Create `docs/{printer-integration-qualification.md,printer-startup-profile.md}`; add only sanitized real status captures under `test_apps/fixtures/printer/` and corresponding regression cases in existing printer tests. Update mappings in report/status modules only where actual captures require it.

**Interfaces:** Produce a measured compatibility record and exact PC-profile procedure, not a fabricated generic G-code snippet. This is the release/enablement gate for automatic Resume; source development and fake tests above can proceed before hardware is available. Monitoring alone is the supported fallback while qualification is incomplete.

- [ ] **Step 1 — Record the prerequisites before live work.** Identify the actual P1S firmware, Bambu Studio version, LAN/developer settings, configured serial, TLS peer identity, and intended serial port if a board is involved. Keep LAN access codes out of the document and fixtures. Obtain execution authorization for live printer commands/board flashing; this plan itself supplies none. Mark each result pending until measured, and keep automatic Resume unavailable while the gate is incomplete.
- [ ] **Step 2 — Establish protocol evidence.** Using status-only monitoring first, capture full/delta reports, ordinary/pause/error/terminal cadence, usable run identity, and repeated identical filenames/local `0` IDs. Sanitize credentials and replace identifiers consistently so delta/identity behavior is retained. Add regression tests from each capture, assert the expected interpretation, and update the mapping only after reproducing any discrepancy as a failing host test.
- [ ] **Step 3 — Qualify the paused PC-prepared job.** Save a separate printer profile with the supported pause command at the earliest demonstrated safe/resumable location. Under supervision, prove it waits before any purge/prime/model extrusion, preserves PC print options, and survives the expected preheat wait. Record any firmware motion/heating before the pause rather than claiming there is none. After the pause has been demonstrated, use an explicitly authorized, separate `build-printer-qualified` / `sdkconfig.printer-qualified` commissioning build with `CONFIG_P1S_PRINTER_AUTO_RESUME=y` to demonstrate one nonretained QoS 0 Resume continues that same job; verify no write replay after forced connection loss. Do not change the default-off source option, race to Pause, or substitute a first-layer pause.
- [ ] **Step 4 — Check the gate.** Every spec section 4 qualification item plus usable job association and native one-second dispatch behavior must have evidence. If any fail, leave automated release unavailable, keep monitoring, and report the exact incompatibility for a workflow/transport decision. If all pass, record the qualified command/profile/version combination, opt-in build configuration, and exact setup/requalification instructions. Generic images keep the option off; only the qualified configuration enables this feature. Update pending fixture tests to passing measured cases and rerun `printer_(report|status|link|cycle|control)`.
- [ ] **Step 5 — Commit evidence.** Message: `docs: qualify P1S startup pause and resume workflow`. If hardware is unavailable, commit only clearly pending instructions and sanitized synthetic fixtures; leave this task and the enablement gate incomplete.

### Task 11: Whole-feature regression and commissioning handoff

**Files:** Update `README.md`, `docs/commissioning-checklist.md`, and qualification/profile documents with actual results. Add native `test_printer_control.c` integration cases and unit CMake registration where not already covered. Change production code only to fix a reproduced regression.

**Interfaces:** Deliver the existing manual controller plus the qualified optional automation feature. No new thermal setting schema, partition layout, compiled secret, public release, or signing-key change.

- [ ] **Step 1 — Run complete source verification.** Use these commands in the activated environment; require exit 0 and all executed tests passing. Record actual counts rather than copying previous feature counts.

  ```powershell
  cmake -S test_apps/host -B test_apps/host/build -G 'MinGW Makefiles'
  cmake --build test_apps/host/build
  ctest --test-dir test_apps/host/build --output-on-failure
  node --test test_apps/web/*.cjs
  python -m unittest discover -s test_apps/tools -v
  ./test_apps/tools/test_publish_firmware.ps1
  idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D P1S_BOOTSTRAP_CREDENTIALS=OFF build
  idf.py -B build-bootstrap -D SDKCONFIG=sdkconfig.bootstrap -D P1S_BOOTSTRAP_CREDENTIALS=ON build
  idf.py -C test_apps/unit build
  idf.py -C test_apps/sensor_smoke build
  idf.py -B build-release size
  git diff --check
  ```

  Build the personal bootstrap only with the owner's existing local ignored secret; do not invent, print, or commit credentials. If the file is unavailable, record that build as not run. When Task 10 has qualified its separate opted-in configuration, also build and run `size` for `build-printer-qualified` using `sdkconfig.printer-qualified`; resource/flash verification must include automation enabled, not just a generic disabled image. Check the app image plus required signing/padding allowance against both `0x1F0000` slots using existing packaging validation; do not sign/publish just to complete this plan. A size failure requires reducing/reviewing implementation, not expanding partitions.
- [ ] **Step 2 — Run native integration only when authorized.** With an explicitly selected board/port and the project's backup/restore procedure, execute the existing Unity runner plus new printer API/control cases using fake transport. Never select the first available COM port automatically. Confirm independent runtime expiry, urgent stop, configuration races, boot-disabled behavior, and four SSE clients with polling. Restore the approved normal image/settings after test firmware; do not report an unexecuted native suite as passed.
- [ ] **Step 3 — Measure resource and end-to-end behavior.** After Task 10 passes, follow the existing unloaded-relay commissioning order before supervised heated testing. Measure free/largest-block heap and task stack margins with MQTT/TLS, maximum valid report, four SSE clients/polling, and OTA admission/suspension. Demonstrate the full preheat -> one Resume -> hold -> actual finished -> heater-off/cooldown sequence, then cancel/error/later-pause/status-loss and uncertain-dispatch cases. Reboot/reconnect and prove no heat or Resume is restored. Recheck ordinary manual heating through Wi-Fi loss and all existing thermal fault behavior.
- [ ] **Step 4 — Record limitations and hand off.** Update README/API/setup/profile/commissioning documents with the qualified versions, measured timing/resources, and actual checks run. Distinguish source completion, native test completion, printer qualification, and heated commissioning. If any physical step remains unavailable, leave its checkbox unchecked and keep automatic release unavailable rather than calling the complete feature commissioned. Stage only this task's fixes/docs and commit as `docs: document verified printer automation and commissioning`.

## Self-review and dependency check

Reviewed against the spec before handoff:

- Spec sections 1-4 (owner scope, alternatives, workflow, qualification) map to Tasks 4, 6, 9, 10 and the explicit release gate.
- Section 5 (MQTT, delta status, TLS/time) maps to Tasks 1 and 5; section 6 (run/token association) to Tasks 1, 4, 6, 8.
- Sections 7-9 (cycle, independent safety, service failures) map to Tasks 3, 4, 6, 7 and their injected/race tests.
- Section 10 (storage, components, APIs, dashboard) maps to Tasks 2, 5-9; sections 11-12 to Tasks 10-11.
- All five Review Focus items have named assertions in their owning tasks. No live/hardware result is assumed.
- Types and API names above are shared across task Interfaces blocks; redacted status remains separate from credential-bearing configuration.
- Tasks 1-3 establish contracts; Task 4 uses 1/3; Task 5 uses 1/2; Task 6 uses 1-5; Task 7 uses 6; Task 8 uses 6/7; Task 9 uses 8. Tasks 10-11 gate real-world enablement/release readiness, not the ability to write source against fakes.

## Execution handoff

The owner selected **Native** execution on **main**. Source tasks run in this session with regression tests, followed by one independent whole-change review. Physical qualification and commissioning remain separate pending work; see the qualification record for the evidence still required.

Reference for the pinned client: [Espressif MQTT 1.1.0](https://components.espressif.com/components/espressif/mqtt/versions/1.1.0/readme); [ESP-MQTT retransmission behavior](https://docs.espressif.com/projects/esp-mqtt/en/latest/esp32/#mqtt-message-retransmission). These inform the adapter tests, not a claim that an untested native write deadline or the owner's printer has already been qualified.
