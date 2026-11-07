# P1S integration qualification

Status: **historical — Preheat & Resume was replaced by Monitor P1S on 2026-11-08.**

> Monitor P1S never sends Resume and needs no Developer Mode, startup pause or
> time server ([design](superpowers/specs/2026-11-07-monitor-p1s-design.md)).
> The Resume qualification below no longer applies; its recorded evidence is
> kept as history. Live Monitor P1S trials are tracked in the
> [commissioning checklist](commissioning-checklist.md).

On 2026-11-01 the owner reported that the installed P1S firmware and Bambu
Studio versions are unknown and will be supplied later. “Probably latest” is
not a measured version. At that point, no printer connection, live print command,
board flash, relay measurement, or heated trial had been performed. A subsequent
monitoring-only session is recorded below; automatic Resume remains unqualified.

Native ESP-IDF source, injected host tests, browser tests, and compiled Unity
images establish source behavior. They do not qualify the printer workflow.
On 2026-11-07 the owner requested removal of the build-time qualification gate.
It has been removed from the current source. Runtime checks require a live
printer, fresh paused status and healthy chamber. The later same-day request
also removed job/startup-history and checkbox admission, plus the settling
delay: pressing the button resumes immediately at/above target, or heats
until target then sends Resume. See the
[current paused-target design](superpowers/specs/2026-11-07-paused-target-resume-design.md).
Connection loss prevents arming
and ends active automation. Connecting never starts a cycle by itself.
Earlier build and device observations below describe the older gated image.

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
Automatic Resume was off in both configurations at that time. Runtime resource
measurements remain pending; current source no longer has a build-time gate.

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
additional defects. Automatic Resume was disabled in that generic build;
the Unity image was compiled only, and no board or printer was exercised.

For an existing cached build, run `idf.py reconfigure` to discover the printer
component's MQTT Kconfig. Keep the custom event queue at four slots and API
locks enabled. If an unreadable/newer optional printer record blocks saving,
the dashboard's deliberate **Forget printer** action is available even when
setup shows unconfigured; it clears only that printer record.

## Record before live work

| Item | Recorded value |
|---|---|
| P1S firmware | SSDP advertised `01.10.00.00` on 2026-11-07 |
| Bambu Studio version | Saved Studio config header `02.08.02.61`; running executable version not independently measured |
| Printer LAN/developer authorization settings | SSDP advertised cloud mode/occupied binding; monitoring worked without changing modes; Developer Mode not separately queried |
| Printer serial and configured IPv4 | Recorded locally; discovered serial matched Studio's selected printer; controller configured with the current LAN IP; no access code in evidence |
| TLS certificate issuer, validity and serial identity | Positive verified ESP32 TLS/telemetry succeeded; certificate issuer/dates and negative cases not separately captured |
| Reachable NTP host / synchronized clock | `pool.ntp.org` configured; verified ESP32 TLS succeeded; actual wall clock/sync state not exposed by status API |
| Board and explicitly selected serial port | New USB serial COM9, 115200; application and physical EN boot capture confirmed ESP32 rev 3.1 / 4 MB |
| Authorization for printer commands / board flashing | Owner authorized connection/configuration monitoring and later signed USB application upload on 2026-11-07; no controller Resume or chamber-enable command sent; upload performed without a tool-issued reset |
| Firmware source revision and build configuration used | Earlier monitoring used app `1.0.0`, compile time Nov 7 2026 01:40:23, ELF SHA prefix `40f4a750b`; updated generic application uploaded as recorded below, with application boot pending |
| Startup profile name, export and checksum | Pending |

The implementation uses ESP-IDF 6.1 and ESP-MQTT 1.1.0. TLS requires the embedded
Bambu CA bundle and the configured printer serial as the expected certificate
name. Credentials use separate MQTT fields; the access code is never placed in
the broker URI or public printer status. The bundle provenance is
[OpenBambuAPI's CA bundle](https://github.com/Doridian/OpenBambuAPI/blob/main/examples/ca_cert.pem).
Protocol context: [local MQTT](https://github.com/Doridian/OpenBambuAPI/blob/main/mqtt.md)
and [TLS identity](https://github.com/Doridian/OpenBambuAPI/blob/main/tls.md).

## Live monitoring — 2026-11-07

The owner changed Wi-Fi through the setup dashboard. Serial confirmed the
station address and setup AP shutdown. Local discovery found the P1S, whose
serial matched Studio's saved selected printer. Its saved LAN access code was
read locally without printing it and submitted once to the controller's printer
configuration API; status confirmed the configuration was saved.

The ESP32 established verified MQTT/TLS and received fresh, complete idle
status with the temperature sensor fault still latched. Heater, fan and manual
fan remained disabled, `can_arm` remained false and Resume was not attempted.
The observation caught intermittent stale telemetry despite `connected: true`;
31 samples over 300.6 seconds all stayed connected with a valid baseline, but
three were stale and 28 were fresh. No reboot or active output was observed
through the status API. Continuous freshness is not a pass. The controller reported about
-87 dBm Wi-Fi signal. This is an observed limitation, not a confirmed cause of
the telemetry gaps. A separate PC-side TLS diagnostic timed out before
subscription, so it did not establish the printer's report cadence.

See the [connection test and evidence](printer-connection-test.md). No printer
job, cloud/developer-mode change, heater trial or Resume qualification occurred.

Later in the same session, live sensor status became valid and fault-free,
and the owner reported both relay loads remained disconnected. The protected target
API saved 23.5 degrees C below the then-current sensor temperature. Heating
remained disabled. The owner started and manually paused a job from the
printer app while the controller was observed: preparing, running, and fresh
complete paused status were captured. Final sensor status was valid at
23.625 degrees C with no fault. No preheat cycle was armed and no Resume was
attempted because the installed image still reported `qualification_required`.
The manual pause does not qualify the pre-extrusion startup-pause placement.

## USB application upload — 2026-11-07

The owner requested upload only if the board was already in bootloader mode,
with no reset afterward. A no-reset probe confirmed the expected ESP32 rev 3.1
on COM9. After serial-read errors, the owner power-cycled the board and requested
115200 baud. The final upload stayed at that speed, with all automatic reset
and write-retry reset paths disabled.

The newly signed generic 1.0.0 application (1,245,172 bytes) was written only to
the existing selected `ota_0` slot at `0x20000`. Flash digest verification and
full byte-for-byte readback passed. Its SHA-256 is
`2ce9002b0eb6b511f2e882e8f9d5176b2eef29d53bb0b0415e04d6c9deb609bd`.
Backups cover the boot/settings prefix and every application sector changed.
The complete prefix through `0x20000` matched afterward, preserving bootloader,
partition table, Wi-Fi/printer settings and OTA selection. Readback used
individually MD5-checked 4 KB transactions; seven failed reads were retried
without resetting. Serial integrity remains an observed connection limitation.

Evidence is in the ignored local
`.superpowers/printer-resume-upload-20261107/flash-verification.json`; private
backup bytes and credentials are not included in public evidence. The tool
closed COM9 with the board still in the RAM flasher stub/download mode. It did
not start the new application, arm a cycle, enable heat or send Resume. The
owner subsequently booted the application as recorded below. Upload
verification is not printer workflow qualification.

## Post-upload boot and readiness — 2026-11-07

After the owner left download mode, the dashboard APIs were reachable and
reported firmware 1.0.0 with `trial_pending: false`. Saved Wi-Fi/printer
configuration was restored, printer monitoring was connected with fresh,
complete status, and the sensor was valid without a latched fault. Heat,
heater, fan and manual fan were disabled; cycle ID stayed zero and
`resume_attempted` stayed false throughout the read-only observation.

The existing job initially reported paused at 0%. `can_arm` remained false
with `identity_required`, including beyond one complete five-minute recovery
interval from the first connected observation. A brief running report was
followed by paused status again, without any controller Resume attempt.
The controller's identity rule requires a usable job start time or observation
of idle/terminal followed by a newly submitted job on the same connection.
Restarting the ESP32 while a job is already paused loses the latter history;
resuming and pausing that same run does not restore it. A fresh test run,
started after connected idle monitoring is established, is the next test.

The saved target remains 23.5 degrees C. During this observation the valid
sensor rose from about 24.75 to 25.875 degrees C, outside that target's
one-degree settling band by the end. The unloaded test target should be
chosen against a fresh sensor reading before a later cycle is armed. Wi-Fi
RSSI was approximately -93 dBm; this does not explain away the explicit
job-identity block.

Sanitized local evidence is in
`.superpowers/printer-resume-upload-20261107/post-boot-readiness.json` and
`post-boot-refresh-readiness.json`. Only GET requests were made. No raw MQTT
reports were captured and no automatic Preheat/Resume cycle was exercised.

## Paused-state threshold update — 2026-11-07

The owner subsequently confirmed that a new print stayed connected throughout
download and pause, yet the old image still blocked Preheat/Resume. They chose
live paused status as the printer admission rule and requested immediate Resume
at or above target, with heating until target when below. This supersedes the
identity-history and settling instructions in the historical observations above.

The updated source passed 147 host, 43 browser and 19 packaging tests; release
and Unity images compiled. Unity was not run on hardware. A fresh independent
review identified a benign-report race at native dispatch; failing regressions
reproduced it before the fix. New tests cover optional start-time enrichment,
while real run/pause transitions, disconnect and Stop still revoke old requests.
Profile, layout, image signature/key and HTTP arm stack checks passed.

The unloaded test target was changed through the protected API to 24.5 degrees C,
below the then-valid sensor reading of 25.375 degrees C. Heating and outputs
remained disabled; no cycle was armed and no Resume was sent by this change.

The owner also reported an earlier download stall that continued immediately
when only the USB/FTDI-powered ESP32 was switched off. A later job downloaded
quickly with the controller continuously connected. No raw protocol capture or
controlled reproduction establishes the cause, and the software tests do not
establish that the download interaction is fixed.

After the owner reconnected the board in BOOT mode, the signed threshold image
was uploaded on COM9 at 115200 baud to `ota_0` at `0x20000`. Flash digest and
full byte-for-byte readback passed for 1,245,172 bytes, SHA-256
`2391afb63e25e0080b5952f7641292d3babbd9bdec5d4395adb2266a67c5c0a6`.
The complete prefix through `0x20000` matched its backup afterward, preserving
bootloader, partition table, settings and OTA selection. Four serial-read
integrity retries recovered without resetting. The port was closed with the
board in the RAM flasher stub/download mode; the application was not started.
Local evidence is in the ignored
`.superpowers/sdd/2026-11-07-paused-target-resume/flash-verification.json`.
First boot and the physical paused/temperature/Resume test remain pending.

## Evidence still required

Rows remain pending or partial. Record date, operator, versions, method,
sanitized capture paths, and actual results before changing a row to pass.

| Check | Required evidence | Result |
|---|---|---|
| Verified status-only connection | Correct CA/name succeeds; wrong name/CA or unsynchronized time cannot produce eligible status | Partial: positive ESP32 verified connection and fresh complete idle status on 2026-11-07; negative cases pending |
| Full and delta reports | Field names, cadence, missing fields, pause, running, error, cancel and finish traces | Partial: idle baseline observed with intermittent stale periods; raw report captures, cadence and job transitions pending |
| Paused admission without identity | Already-paused/later-layer print with missing or local `0` IDs can be deliberately armed | Host/native fixtures pass; physical test pending |
| PC job acceptance | Prepared job accepted with all selected print, plate, filament and calibration options preserved | Pending |
| Optional early resumable pause | If wanted, validate the separate PC profile before all purge/prime/model extrusion | Pending; optional for the button |
| Temperature threshold | Below target stays paused and heats; at/above target sends one immediate Resume, without a settling timer | Host/native fixtures pass; physical test pending |
| Direct LAN Resume permission | Current printer authorization settings permit the command | Pending |
| One live attempt | One QoS 0, nonretained Resume continues the same job; no replay after disconnect/reconnect | Pending |
| Native send deadline | Queue wait, runtime guard, socket backpressure, partial writes and forced disconnect respect the absolute 1,000 ms budget; no late queued write | Pending |
| Revocation during send | Stop, Disable, fault and expired ownership prevent a waiting send; no stale request revives heat | Pending |
| Terminal behavior | Actual finish, failed/cancelled state, later pause, new observed run and stale status end chamber control | Pending |
| Reboot | Configuration restored; heat and pending Resume remain disabled | Partial: post-upload owner boot restored configuration and fresh monitoring with heat/Resume inactive; active-cycle reboot injection pending |
| Resources | Free/largest-block heap and task-stack margins with MQTT/TLS, 32 KiB report, four SSE clients plus polling, and idle OTA suspension | Pending |
| Thermal commissioning | Follow the existing unloaded-relay stages before supervised heated trials | Pending |

No real printer captures have been collected. Current protocol fixtures are
synthetic and explicitly labeled in `test_apps/fixtures/printer/README.md`.
Replace identifiers consistently across related captures, preserve field
presence and ordering of events, and omit credentials. Add each measured case
as a regression test before changing report mappings.

## Runtime availability and live validation

Collect outstanding compatibility evidence during supervised unloaded-relay
tests before heated use. If a separate pre-extrusion startup profile is wanted,
qualify its placement; that profile is optional for the paused-printer button.

All current builds contain Preheat/Resume; there is no qualification option or
separate feature-enabled image. The dashboard enables arming only while the
connection, fresh paused state and chamber checks pass. At/above target sends
Resume immediately; below target heats until reached. Perform supervised
Resume and resource tests with relay loads disconnected before heated trials.
Record the exact successful firmware/slicer/profile combination in this
document and in the [startup profile](printer-startup-profile.md).
Requalify after printer firmware, authorization-mode, slicer startup, or relevant
transport changes.
