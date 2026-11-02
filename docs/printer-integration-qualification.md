# P1S integration qualification

Status: **pending — automatic Resume is disabled in generic builds.**

On 2026-11-01 the owner reported that the installed P1S firmware and Bambu
Studio versions are unknown and will be supplied later. “Probably latest” is
not a measured version. No printer connection, live print command, board flash,
relay measurement, or heated trial was performed for this integration.

Native ESP-IDF source, injected host tests, browser tests, and compiled Unity
images establish source behavior. They do not qualify the printer workflow.
`CONFIG_P1S_PRINTER_AUTO_RESUME` defaults to `n`; monitoring and connection setup
remain available. There is no dashboard switch that bypasses this build gate.

## Source verification — 2026-11-01 to 2026-11-02

These checks ran on Windows using native ESP-IDF 6.1, without connecting to a
printer or flashing a board.

| Check | Actual result |
|---|---|
| MinGW/CMake host suite | 41/41 passed |
| Browser suite | 28/28 passed |
| Python packaging tests | 10/10 passed |
| Publisher shell fixtures | 4/4 passed; external publication mocked |
| Generic release build and `size` | Passed; unsigned app 1,245,104 bytes |
| Personal bootstrap build | Passed using the existing ignored local configuration; unsigned app 1,245,104 bytes |
| Native Unity image build | Passed; 797,456 bytes; not flashed or executed |
| Sensor smoke image build | Passed; 159,296 bytes; not flashed or executed |
| Whitespace validation | `git diff --check` passed |
| Dashboard preview | Desktop and 390 px viewport inspected with synthetic status; no horizontal overflow or JavaScript errors |

The source tests cover bounded/delta reports, run association, token and cycle
races, independent heat-lease expiry, Stop/Disable, transport deadlines,
configuration/storage, HTTP admission, lost replies, and OTA/Wi-Fi coexistence.
Native Unity cases also compile the real coordinator with fake printer/heat
boundaries and exercise HTTP/SSE setup; their execution on ESP32 is pending.

The MQTT adapter reserves four event slots and keeps API locks enabled. A build
guard rejects insufficient capacity. Host adapter tests exercise both early
and ordinary subscription acknowledgements; the native queue-capacity case is
compiled but has not been run on ESP32.

An independent whole-change review identified six source defects. Each was
reproduced before its fix: missing client identity on MQTT custom events;
adverse report transitions lost when deltas were merged; missing final thermal
readiness checks; a send revision newer than the observation actually validated;
an excessive HTTP arm stack path; and unavailable dashboard recovery for an
unreadable optional printer record. All six are fixed with regression coverage.
Report history now preserves later pauses, transient errors/HMS and identity
changes; manual advance during preheat ends automation. Harmless diagnostic
updates retain the current offer. The final coordinator and native send guards
both check the captured target and ±1 °C band after waits. No review findings
remain deferred.

The compiled HTTP arm path originally required at least 4,160 bytes against a
4,096-byte server stack. Removing redundant by-value input and rollback copies
reduced its call frames to 2,624 bytes. With 304 bytes of ABI/interrupt overhead
and a required 512-byte margin, the checked path needs 3,440 bytes and passes in
both release and bootstrap builds. Run `python tools/check_printer_stack.py --build build-release`
after building to repeat this read-only check. It guards
this known path; it does not replace the pending runtime high-water and heap
measurements below.

The existing packaging validators confirmed the unchanged partition layout,
generic hardware/security profile and absence of embedded generic Wi-Fi
credentials. Both app identities were checked (generic flags `0`, personal
bootstrap flags `3`), and ESP32 image verification passed. Both images are
16-byte aligned. Adding the existing 68-byte ECDSA V1 packaging allowance gives
1,245,172 bytes, leaving 786,444 bytes in **each** `0x1F0000` OTA slot. No image
was signed, packaged for release, published, or flashed for these checks.
Automatic Resume is off in both configurations. An opted-in qualification image
and its runtime resource measurements remain pending.

A follow-up review on 2026-11-02 reproduced two additional defects: missing
baselines were never retried after the subscription-time snapshot cooldown,
and a successful arm could race a poll's earlier inactive-lease snapshot.
Regular link ticks now retry missing baselines within the same 300-second
attempt budget, and the coordinator captures runtime inputs under the arm
admission lock. The new regressions failed before their fixes. Follow-up
verification passed 45/45 host tests, 28/28 browser tests, 10/10 packaging
tests, and 4/4 publisher fixtures. Generic release and native Unity images
rebuilt successfully; the HTTP arm stack check still requires 3,440 of 4,096
bytes including its margin. An independent source review found no additional
issues in these fixes. No printer or board execution was performed.

A subsequent review on 2026-11-02 reproduced and fixed three further issues.
Resume now carries the armed cycle's safety history through the final link
validation, so transient error/HMS or identity changes cannot be erased by a
later healthy projection. Initial snapshots are preserved, and active jobs
with incomplete identity recover within the same 300-second attempt budget;
complete idle status does not cause repeated requests. Forget printer now
preserves replacement fields or access codes entered while its reply is pending.
The adverse dispatch, missing-identity and draft-loss regressions failed before
their fixes. Final verification passed 56/56 host tests, 31/31 browser tests,
10/10 packaging tests and 4/4 publisher fixtures. Generic release and native
Unity images built successfully; the HTTP arm stack check remains 3,440 of
4,096 bytes including its margin. An independent read-only review found no
additional defects. Automatic Resume remains disabled in the generic build;
the Unity image was compiled only, and no board or printer was exercised.

For an existing cached build, run `idf.py reconfigure` to discover the printer
component's MQTT Kconfig. Keep the custom event queue at four slots and API
locks enabled. If an unreadable/newer optional printer record blocks saving,
the dashboard's deliberate **Forget printer** action is available even when
setup shows unconfigured; it clears only that printer record.

## Record before live work

| Item | Recorded value |
|---|---|
| P1S firmware | Pending owner information |
| Bambu Studio version | Pending owner information |
| Printer LAN/developer authorization settings | Pending inspection |
| Printer serial and configured IPv4 | Pending; do not include access codes |
| TLS certificate issuer, validity and serial identity | Pending live verification |
| Reachable NTP host / synchronized clock | Pending live verification |
| Board and explicitly selected serial port | Pending; never choose a port automatically |
| Authorization for printer commands / board flashing | Not provided for this work |
| Firmware source revision and build configuration used | Record when testing |
| Startup profile name, export and checksum | Pending |

The implementation uses ESP-IDF 6.1 and ESP-MQTT 1.1.0. TLS requires the embedded
Bambu CA bundle and the configured printer serial as the expected certificate
name. Credentials use separate MQTT fields; the access code is never placed in
the broker URI or public printer status. The bundle provenance is
[OpenBambuAPI's CA bundle](https://github.com/Doridian/OpenBambuAPI/blob/main/examples/ca_cert.pem).
Protocol context: [local MQTT](https://github.com/Doridian/OpenBambuAPI/blob/main/mqtt.md)
and [TLS identity](https://github.com/Doridian/OpenBambuAPI/blob/main/tls.md).

## Evidence still required

Every row is pending. Record date, operator, versions, method, sanitized capture
paths, and the actual result before changing a row to pass.

| Check | Required evidence | Result |
|---|---|---|
| Verified status-only connection | Correct CA/name succeeds; wrong name/CA or unsynchronized time cannot produce eligible status | Pending |
| Full and delta reports | Field names, cadence, missing fields, pause, running, error, cancel and finish traces | Pending |
| Run identity | Distinct runs with the same filename and local `0` IDs; meaningful start identity or continuous terminal-to-new-job history | Pending |
| PC job acceptance | Prepared job accepted with all selected print, plate, filament and calibration options preserved | Pending |
| Early resumable pause | Pause before **all** purge, prime and model extrusion; record any earlier motion/heating | Pending |
| Preheat wait | Job remains safely paused for the intended wait; no unrecoverable printer timeout | Pending |
| Direct LAN Resume permission | Current printer authorization settings permit the command | Pending |
| One live attempt | One QoS 0, nonretained Resume continues the same job; no replay after disconnect/reconnect | Pending |
| Native send deadline | Queue wait, runtime guard, socket backpressure, partial writes and forced disconnect respect the absolute 1,000 ms budget; no late queued write | Pending |
| Revocation during send | Stop, Disable, fault and expired ownership prevent a waiting send; no stale request revives heat | Pending |
| Terminal behavior | Actual finish, cancel, error, later pause, identity change and stale status end chamber control | Pending |
| Reboot | Configuration restored; heat and pending Resume remain disabled | Pending |
| Resources | Free/largest-block heap and task-stack margins with MQTT/TLS, 32 KiB report, four SSE clients plus polling, and idle OTA suspension | Pending |
| Thermal commissioning | Follow the existing unloaded-relay stages before supervised heated trials | Pending |

No real printer captures have been collected. Current protocol fixtures are
synthetic and explicitly labeled in `test_apps/fixtures/printer/README.md`.
Replace identifiers consistently across related captures, preserve field
presence and ordering of events, and omit credentials. Add each measured case
as a regression test before changing report mappings.

## Enablement decision

Keep monitoring only if any required item is unknown or fails. In particular,
an ordinary first-layer pause does not prove that startup purge/prime extrusion
was prevented. Do not substitute an automatic race to Pause.

After the startup pause has been proved, use a separately authorized
`build-printer-qualified` / `sdkconfig.printer-qualified` commissioning build
with `CONFIG_P1S_PRINTER_AUTO_RESUME=y` for supervised Resume and resource tests.
Keep generic builds at `n`. A build with the option enabled is not evidence of
qualification by itself. Record the exact successful firmware/slicer/profile
combination in this document and in the [startup profile](printer-startup-profile.md).
Requalify after printer firmware, authorization-mode, slicer startup, or relevant
transport changes.
