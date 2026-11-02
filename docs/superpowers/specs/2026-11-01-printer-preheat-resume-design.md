# P1S status, chamber preheat, and print resume

Date: 2026-11-01

Status: design accepted for implementation; source implementation is on main.
Printer communication and the startup pause have not been tested on the owner's
P1S. Automatic Resume remains disabled pending physical qualification. Source
verification does not establish hardware compatibility.

## 1. Intent and agreed direction

The PC prepares the print, including slicing, filament/AMS choices, plate
selection, calibration, and the ordinary print submission. The ESP32 controls
the external chamber heater, observes the P1S, and releases a deliberately paused
job once the chamber is ready. At print completion it disables chamber heating.

The owner explicitly does not want file selection or print settings on the
ESP32. It must not upload files, slice models, maintain a print queue, or rebuild
a Bambu Studio print request.

An uploaded file is not a prepared, paused print. There is no documented generic
command meaning "start whichever file was open on the PC." The selected
direction therefore uses the existing MQTT `print.resume` command for a job
that the PC has already submitted and the printer has paused intentionally.
This is an important change from the earlier upload-then-start proposal.

The agreed requirements are:

- Keep all print preparation and print options on the PC/printer.
- Read printer status on the ESP32.
- Preheat the chamber before releasing the waiting print.
- Maintain chamber temperature during that print.
- Turn the external heater off when the print finishes.
- Preserve the existing independent DS18B20 and relay protection.

The arming interaction, timing defaults, failure policies, and architecture
below are proposed engineering choices, not additional requirements attributed
to the owner.

## 2. Approaches considered

| Approach | Benefit | Cost or reason not selected |
| --- | --- | --- |
| PC submits the complete job with an intentional startup pause; ESP32 sends Resume | Printer retains every print option; ESP32 needs no file selection or print setup | Requires a reliable startup pause and an explicit association with the waiting job; selected direction |
| PC uploads a file; ESP32 sends a new `project_file` request | ESP32 can launch a stored job directly | ESP32 must identify the file/plate and supply launch options; the owner rejected this responsibility |
| PC waits for chamber readiness and submits the print itself | Keeps all job handling on the PC | Requires a PC helper and the PC to remain available; does not meet the requested ESP32 release of a prepared job |

Do not implement automatic immediate-pause polling as a substitute for the
startup pause. A network command racing the start of a print cannot guarantee
that preparation or extrusion has not already happened.

## 3. Operator workflow

1. Once during setup, configure the printer connection on the existing ESP32
   dashboard. Establish and test a dedicated Bambu Studio printer profile with
   an intentional startup pause as described in section 4.
2. On the PC, prepare the job, choose the normal print options, and submit it
   using that profile. The P1S accepts the job and reaches the intentional pause.
3. On the ESP32 dashboard, check the displayed waiting job and chamber target.
   Press **Preheat & Resume**, explicitly confirming that this is the intentional
   startup pause. No filename, AMS mapping, or print options are entered there.
4. The ESP32 enables chamber regulation, waits for the readiness condition, and
   makes one Resume attempt for this armed cycle.
5. The printer continues the already submitted job. The ESP32 maintains the
   captured chamber target and observes progress.
6. A matching finished status disables chamber heating and starts the existing
   normal disable fan cooldown. This consumes the cycle; the next print needs
   a new explicit arm action.

The explicit arm action is a proposed v1 default. Merely connecting to a paused
printer, seeing `PAUSE`, or reaching the temperature must never arm or resume a
job. Once the printer has accepted the job, the PC is not part of the ongoing
ESP32 control loop.

## 4. Printer and PC prerequisites

The PC, printer, and ESP32 need connectivity on the same local network. Configure
the P1S LAN address, serial number, and LAN access code. A DHCP reservation is
recommended so the configured printer address stays usable.

Record the actual P1S firmware and Bambu Studio versions during commissioning.
On firmware with authorization controls, qualify the direct LAN command path
with LAN-only and Developer Mode as applicable. Reading status does not prove
that Resume is permitted. LAN-only mode affects Bambu cloud/Handy connectivity;
the design does not bypass printer authorization or use Bambu account tokens.

### Startup pause is a qualification requirement

Create a separate saved PC printer profile rather than modifying the stock
profile for every print. Place a supported, resumable pause in its startup
G-code. Determine the exact pause command and placement against the installed
P1S firmware; this spec does not assert an untested command works at an arbitrary
startup position.

The qualification must demonstrate that:

- The job is accepted with its PC-selected print options intact.
- The intentional pause occurs before extrusion, including purge/prime
  extrusion, and leaves the printer in a state that can safely wait for preheat.
- Its print/job metadata and paused status are observable over LAN MQTT.
- MQTT Resume continues the same job through normal preparation and printing.
- A long enough wait for the chamber does not cancel the job or create an
  unrecoverable printer timeout.

Some firmware-controlled preparation may occur before user startup G-code;
do not promise that the printer will perform no motion or heating beforehand.
If a suitable pause before extrusion cannot be demonstrated, this design's
automated release is not qualified. Keep status monitoring available, report
the incompatibility, and revisit the workflow rather than silently substituting
a first-layer pause or a race to issue Pause.

The saved profile and its tested instructions are part of the eventual feature
deliverable. An ordinary unmodified print profile remains available for prints
that do not use chamber automation. Recheck the profile after printer firmware
or relevant slicer startup changes.

## 5. Local MQTT connection and status model

Use ESP-MQTT over verified TLS to the configured printer on port 8883, with
username `bblp` and the configured LAN access code as password. Use a distinct
ESP32 client ID and a clean session.

- Subscribe to `device/<serial>/report`.
- Publish requests only to `device/<serial>/request`.
- Allow only status snapshot requests and the one armed Resume operation in
  the v1 application command path. No printer stop, temperature, fan, movement,
  calibration, new-print, or arbitrary G-code commands are exposed.
- After subscription, request a snapshot using the `pushing` object with
  `command: "pushall"`, a sequence ID, `version: 1`, and `push_target: 1`.
- Request snapshots on initial connection and when a new baseline is needed;
  rate-limit them to at most one per 300 seconds, including reconnect storms.
  Listen for pushed updates between requests. Do not repeatedly poll `pushall`.
- Disable retained requests. Resume must not enter an offline queue or survive
  a reconnect. Drop messages from an old connection generation.

P1-series status reports can contain only changed fields. Merge valid deltas
into a snapshot; omission is not zero, idle, or field deletion. Invalidate the
baseline on disconnect/reconnect and rebuild the required fields before
allowing an arm action. A valid baseline may be assembled from fresh reports
on the new connection; do not depend on every report being a full snapshot.

Project these fields into bounded internal types:

- `print.gcode_state`: distinguish idle, preparing, running, paused, finished,
  failed, and unknown. Qualify the exact wire-value mapping on the printer.
- Job/run metadata when present: `gcode_start_time`, `task_id`, `subtask_id`,
  `gcode_file`, `subtask_name`, and print type.
- Progress and remaining time, with unknown distinct from a numeric zero.
- `print_error` and the HMS entries required to inhibit unsafe release.
- Bed/nozzle temperatures for display only; they never replace the DS18B20
  chamber measurement.

Track connection generation, baseline validity, report receive time, current
snapshot revision, and individual field validity. MQTT PING/PUBACK packets,
unrelated replies, malformed JSON, and retained data do not refresh live print
telemetry. A valid print delta can refresh telemetry even when unchanged state
is omitted. The default telemetry timeout is 30 seconds.

Assemble fragmented MQTT payloads with checked offsets and an explicit size
limit. The proposed initial maximum is 32 KiB per report, subject to measured
ESP32 heap use. Use bounded parsing/projection, reject malformed or conflicting
decision fields, and never truncate a message into a valid control decision.
Oversized/unparseable reports do not refresh the automation lease. Unknown
state values cannot authorize Resume.

### TLS and time

Trust the Bambu printer CA and verify the peer identity against the configured
printer serial, including the certificate name/SNI behavior when connecting by
IP address. Do not disable certificate or peer-name verification to get a
connection working.

Reuse/factor the existing OTA time initialization into shared, idempotent time
setup. The proposed default NTP source is the existing `pool.ntp.org`; permit a
configured LAN NTP server for a network without internet access. With no usable
clock, do not authorize a TLS session by ignoring certificate validity. Display
a time/connection setup error and keep automation unavailable. Bambu Cloud is
not part of this connection.

## 6. Arming and association with one print

Arming requires all of the following in one coherent admission check:

- Configured connection, verified TLS, a valid baseline, and fresh live status.
- A paused job with no model layers printed, where this firmware exposes that
  evidence. If it does not, the documented startup-pause qualification must
  establish the observable equivalent; progress percentage alone is insufficient.
- Explicit per-cycle operator confirmation that this is the intentional
  startup wait. This confirmation is necessary because generic `PAUSE` does
  not distinguish a planned pause from a later manual pause or an error.
- Sufficient observed identity to associate the cycle with this submitted run.
  A filename or task ID equal to `0` is not a unique run identifier.
- No nonzero active print error or unclassified/blocking HMS condition.
- Healthy, fresh chamber samples, at least three consecutive valid readings,
  a nonzero valid target, no latched fault, and no maintenance operation.
- Chamber heating is currently disabled and no other automatic cycle owns it.
- Successful acquisition of the service-operation gate.

Capture a boot-local cycle ID, connection generation, target, job fingerprint,
arming snapshot revision, and monotonic deadline. Job metadata is read-only
association data, not a print setting supplied by the ESP32.

Qualify the job fingerprint from actual traces. Prefer a meaningful run start
identifier together with the job label/file and any usable task IDs. If the
firmware does not expose a usable run identifier, require the ESP32 to have
continuously observed an idle/terminal state, a newly submitted job, and its
initial pause on this connection before arming. Lose that observation history
on any disconnect. If neither method can distinguish the submitted run, disable
automated Resume rather than guess. Treat a visible identity change or an
unexplained restart of the job as cancellation of the chamber cycle.

Admission consumes a one-use arm token produced for the displayed eligible
job, target, and connection. Its proposed lifetime is 60 seconds. Repeated
status reads retain the current token while these eligibility facts remain
valid; harmless telemetry deltas must not rotate it on every poll. Changes to
the job, target, connection, error state, or eligibility invalidate it
immediately. A repeated browser request must not create another cycle. All
later events must match the captured connection/job association. Thermal
readiness alone is not authority to release any paused print.

## 7. Cycle behavior and proposed timing defaults

| Cycle state | Behavior and exit |
| --- | --- |
| `ready` | Monitoring only; no automatic heat or Resume permission. Eligible paused job can be armed explicitly. |
| `preheating` | Own the chamber target and regulate normally. Require the same healthy, paused job throughout. Cancel on any disqualifying event or preheat timeout. |
| `resume_pending` | Consume Resume permission before making one live publish attempt. Continue chamber regulation while waiting for matching print progress, for at most 30 seconds. |
| `printing` | Maintain the captured target while the same run reports qualified preparation/running states. Never send another Resume. |
| `completed` | Matching finished status disables heating, consumes the cycle, and uses normal fan cooldown. Retain the outcome for display. |
| `stopped` | Manual stop, cancellation, later pause, or loss of eligibility disables heating and consumes the cycle. |
| `error` | Sensor/runtime failure, printer failure, stale status, or uncertain Resume disables heating and consumes the cycle; show a reason. Existing thermal faults remain latched as before. |

Heating readiness uses the filtered, calibrated chamber temperature from the
existing runtime. Proposed default: reach the captured target at least once,
then remain within target +/-1 degree C for 30 continuous seconds. Falling
outside the band resets the settling timer. The current sample must remain
valid and within its existing two-second age limit. Proposed preheat timeout:
45 minutes from arming, including settling. These are firmware policy constants
for v1, not new print settings or extra UI controls.

Immediately before Resume, recheck the active cycle, captured job/connection,
fresh status, paused state, no printer errors, thermal health/readiness, and
runtime ownership. Invalidate permission before attempting network I/O. Use
live QoS 0 publish without an application/offline queue for this command, so
ESP-MQTT does not automatically retransmit it as an unacknowledged QoS 1/2
message. Qualify that QoS choice on the P1S. If this printer cannot accept it,
resolve transport replay behavior before enabling automation; do not switch to
a reconnecting outbox without reviewing this requirement.

Bound the live send attempt to at most one second. On timeout or revocation,
abort the transport attempt and discard any unsent application request rather
than letting it flush later. A packet already accepted by the network cannot
be recalled; the subsequent status/uncertainty rules still apply.

Network delivery cannot be made exactly-once by an application sequence ID.
Do not retry an uncertain Resume. A successful publish or command reply is
not proof that the printer continued. Require a subsequent, fresh matching
preparation/running status within 30 seconds, or a matching terminal outcome
if the run completes in that interval. If the result is uncertain, disable
heating, show that the printer must be checked, and require operator action.

After release, a later paused state ends chamber automation and disables heat;
it is never automatically resumed. This conservative v1 default also applies
to filament-runout pauses. If the operator resumes using the PC/printer, the
automatic cycle does not silently reactivate. Existing manual chamber control
remains available after the automatic cycle has ended.

Use an actual matching finished state for success, not 100% progress or a zero
remaining-time estimate. A matching failed state, nonzero blocking error,
unexpected idle state, new job, or external cancellation turns heating off.
An unexpected state transition before the ESP32 releases the job also ends the
cycle; for example, manually resuming during preheat must not leave a second
Resume pending.

## 8. Independent heat protection and command ownership

`app_runtime` remains the only owner of relay hardware. Printer code never
drives GPIO25/GPIO26 or changes the pure controller's fault rules. Preserve the
DS18B20 sampling, median filtering, calibration, 0-80 degree C target range,
82 degree C raw/filtered shutdown, minimum restart interval, and fan ordering.

Add explicit manual/automatic command ownership at the runtime boundary. An
automatic cycle gets a revocable heat lease, renewed at most once per second
with a maximum five-second validity and bounded by telemetry/deadline health.
The existing independent 100 ms control loop must enforce expiry even if the
MQTT or automation worker stalls. A full command queue must not prevent lease
revocation or local disable. Fault/maintenance/boot inhibition always outrank
the lease. A suspended worker must not be able to renew an already consumed
cycle by presenting an old cycle ID. Lease expiry invalidates that cycle's
runtime ownership token; waking the worker cannot revive it. Publish the
expired/revoked state through the runtime snapshot so the dashboard can show
the failure even before the automation worker recovers.

Serialize telemetry decisions and Resume dispatch in one automation worker;
check a current runtime readiness snapshot just before dispatch. A remote
command and a later sensor fault cannot be made atomic across two devices.
If a fault occurs during or after Resume, the local heater still shuts off;
this design does not claim to stop a print already accepted by the P1S.

While the cycle is active, reject target changes and manual heat-enable with
409 at both the HTTP and runtime admission boundaries. Always allow local
disable, which revokes the cycle. Preserve the existing independent manual-fan
override. **Stop chamber control** stops chamber heating/automation; it does
not cancel the printer's job. The dashboard must say this plainly.

Normal stop/completion uses the current ten-second disable cooldown. A sensor,
overtemperature, or output fault preserves the current immediate automatic
output shutdown rules; do not add a cooldown that overrides them.

Printer connectivity policy applies only to automatic cycles. Standalone
manual heat regulation continues through Wi-Fi outages as it does today.
No enabled state, lease, arm token, Resume intent, or active cycle survives a
reboot. Reconnection only restores monitoring.

## 9. Failure and service-operation policy

| Event during an active cycle | Required result |
| --- | --- |
| MQTT disconnect or Wi-Fi loss | Immediately revoke automation/heat permission; normal disable cooldown. Never resume or re-enable on reconnect. |
| Silent telemetry gap reaches 30 seconds | Revoke the cycle and disable heat even if the MQTT socket appears connected. |
| Automation worker stalls | Runtime heat lease expires within five seconds, independently of that worker. |
| Sensor/overtemperature/output fault | Existing immediate fault action; consume the cycle; fault acknowledgement does not re-arm it. |
| Printer error, failed job, later pause, or unexpected idle | Disable heat; keep a distinct diagnostic outcome; no printer command is issued. |
| Local Stop chamber control / existing heat Disable | Revoke the cycle and disable heat; printer continues or remains paused as it was. |
| Resume failure or uncertain result | No repeat attempt; disable heat and require checking the printer. |
| ESP32 restart or OTA restart | Outputs start inactive; restore only saved connection configuration. |

Extend the existing atomic service gate with printer-configuration and
printer-cycle owners. Hold the cycle owner from admission through teardown;
release it only when runtime ownership has been revoked. Wi-Fi changes,
printer reconfiguration, and OTA/trial operations cannot be admitted during a
cycle. Return 409 with instructions to stop chamber control first. Likewise,
do not arm while an existing Wi-Fi/OTA/trial/configuration operation owns the
gate. Read-only status and local stop remain available.

Monitoring failure is not a startup failure and printer availability is not
part of OTA trial health. When idle, suspend/release printer transport resources
if needed for OTA to retain the current heap and rollback guarantees. Network
operations, TLS handshakes, JSON work, and NVS writes never run in the
temperature/control tasks or under their snapshot locks.

## 10. Components, persistence, and dashboard contract

Use the existing ESP-IDF 6.1 C architecture, classic ESP32, 4 MB flash, native
HTTP server, and embedded vanilla JS/CSS. Add a printer connection/transport
component, a host-testable automation policy, printer-related shared types,
versioned device configuration, and HTTP/dashboard integration. Keep transport
and policy injectable for host testing.

Store one optional versioned `printer_v1` record in the existing `device_cfg`
partition: enabled flag, IPv4 address, serial, LAN access code, and NTP host.
Do not change the partition table, thermal settings schema, or existing Wi-Fi
record. Validate the complete record before an atomic commit. A failed save
retains the old working configuration. Missing printer configuration is normal;
a corrupt/newer printer record disables this optional integration without
erasing unrelated settings or bypassing existing partition-level NVS faults.

Proposed field bounds: serial 1-32 ASCII letters/digits; access code 1-64 bytes
with no embedded NUL; NTP host 1-253 characters with hostname/IP validation.
Do not silently trim credentials. Keep secrets out of logs, diagnostic history,
status broadcasts, and source/release artifacts. The LAN access code is
write-only through the API; return `access_code_set`, not its value. This
continues the existing trusted-local-network dashboard model without adding
an account/login system.

| Method/path | Contract |
| --- | --- |
| `GET /api/printer` | Bounded snapshot: redacted saved configuration, link/setup reason, baseline/freshness, printer state, observed job label, progress/time, cycle state/outcome, `can_arm`, current arm token when eligible, and existing per-boot CSRF token. No secrets. |
| `PUT /api/printer/config` | Save/connect the complete validated nonsecret fields and optional access code; omitted code preserves it only for the same configured serial. New printer requires a code. 202 means admitted, not connected. |
| `DELETE /api/printer/config` | Clear optional printer configuration while no cycle/service conflict exists. Idempotent; 204 on success. |
| `POST /api/printer/cycle` | Body contains the displayed `arm_token` and `initial_pause_confirmed: true`; atomically arm using the existing target. 202 on admission; repeats/conflicting snapshots cannot start another cycle. |
| `POST /api/printer/cycle/stop` | Idempotently revoke chamber automation and disable its heat; no P1S stop command. 204 once revocation is accepted by the local runtime. |

Apply the existing local Host/Origin checks and per-boot `X-P1S-CSRF` token to
all new mutations. Bound JSON bodies to 1,024 bytes; reject duplicate/unknown
fields, embedded NUL, trailing JSON, invalid types, and invalid values. Use
400 for invalid requests, 403 for origin/token rejection, 409 for a conflicting
operation or ineligible state, and 503 for unavailable local admission.
Configuration/status responses are `Cache-Control: no-store`.

Keep existing `/api/status` and SSE wire contracts intact; do not squeeze
printer data into their current 512-byte status buffer. Use a separate bounded
printer response, initially up to 2,048 bytes, and poll it once per second while
the page is visible. Preserve unfinished configuration edits during polling.
Budget route handlers, sockets, and memory with the existing four SSE clients.

Extend the current dashboard with connection settings and a compact printer
section: connection/freshness, readable print state, observed job name,
progress, chamber target/readiness, and the cycle outcome. Provide the explicit
startup-pause confirmation, **Preheat & Resume**, and **Stop chamber control**.
Disable ineligible actions with a concrete reason. No file browser, upload
control, plate selector, AMS setup, or print-parameter editor is added.
If a mutation response is lost, refresh status; never automatically repeat the
arm/resume operation. Clearly show uncertainty instead of claiming success.

## 11. Verification and acceptance

### Host and browser verification

- Merge fragmented full/delta status correctly, including omitted fields,
  duplicate control fields, invalid types, oversize JSON, unknown states,
  reordered callbacks, wrong topics, retained messages, and old generations.
- Cover valid arm admission, no automatic arming, duplicate/stale arm tokens,
  absent/ambiguous identity, initial-pause history requirements, and job changes.
- Cover exact readiness/settling and 45-minute preheat boundaries, bad/stale
  sensor data, target ownership, and an already warm chamber.
- Prove a cycle can make at most one Resume publish attempt, with no retained
  packet, offline queue, reconnect replay, timeout retry, or later-pause resume.
- Cover matching progress after dispatch, rejected/uncertain Resume, terminal
  status before the progress report, cancellation, error, and new-job races.
- Exercise disconnect/stale-telemetry cutoff, independent runtime lease
  expiry when the worker stops, local disable, fan behavior, service-gate
  races, reboot inhibition, and manual operation through network loss.
- Cover configuration persistence/failure, redaction, request validation,
  status freshness, preserved browser drafts, unknown mutation outcomes, and
  the four existing SSE clients alongside printer polling.
- Run the existing relevant controller, sensor, Wi-Fi, OTA, web, and packaging
  regression checks plus production and unit ESP-IDF builds. Measure flash
  against each existing 0x1F0000 OTA slot and measure worst-case heap during
  MQTT/TLS, maximum reports, four SSE clients, and update admission.

### Physical qualification and feature acceptance

Before operating a heated automatic cycle, capture the actual printer versions,
required modes, verified TLS identity, full/delta status examples, usable run
identity, error/terminal states, and accepted Resume transport behavior.
Demonstrate the startup pause and its placement using supervised printer tests;
do not claim host tests prove printer behavior.

Then commission with relay contacts unloaded before a supervised heated print,
following the project's existing commissioning order. Demonstrate:

1. All job/filament/plate/calibration settings are prepared on the PC; ESP32
   receives no editable print settings and sends no new-print request.
2. Printer waits at the qualified startup pause. Nothing resumes just because
   it is paused, the ESP32 boots, or MQTT reconnects.
3. One explicit arm starts chamber preheat; a simulated/observed ready
   condition causes one Resume and the same job proceeds normally.
4. Completion disables heat with the expected fan behavior. Failure,
   cancellation, a later pause, status loss, and a local stop also disable it.
5. A connection loss during dispatch cannot trigger a second Resume on recovery.
6. Fault recovery, restart, and a subsequent print require new deliberate
   actions; existing manual heating still behaves as documented.

Writing this spec authorizes no flash, live printer commands, mains-load test,
release publication, or deployment. Those are separate execution/commissioning
steps. Do not manufacture compatibility results if no printer is available.

## 12. Builder discretion and remaining measured facts

The builder may choose internal file names, task/queue/stack sizes, a compatible
pinned ESP-MQTT dependency, bounded JSON parsing technique, retry backoff, and
the exact visual placement within the existing dashboard. Keep these within
the contracts and resource limits above.

Installed firmware/modes, exact startup-pause command and location, usable job
identity fields, report cadence/size, and Resume acceptance are measured
compatibility facts to establish during qualification. They are not presumed
owner decisions and must not be filled in with unverified protocol guesses.
If qualification contradicts the selected workflow, report the concrete result
before changing that workflow.

## References

Protocol observations are community-maintained and require qualification on the
installed printer. Sources consulted for this design on 2026-11-01:

- [OpenBambuAPI MQTT protocol](https://github.com/Doridian/OpenBambuAPI/blob/main/mqtt.md):
  local connection, delta reports, `pushall`, Resume, and new-print distinction.
- [OpenBambuAPI TLS guidance](https://github.com/Doridian/OpenBambuAPI/blob/main/tls.md):
  Bambu CA and serial-based peer identity.
- [Bambu authorization and Developer Mode](https://blog.bambulab.com/updates-and-third-party-integration-with-bambu-connect/):
  firmware-dependent third-party control.
- [Bambu Studio pause-G-code support](https://github.com/bambulab/BambuStudio/blob/master/src/libslic3r/PrintConfig.cpp):
  configurable pause behavior; not proof of a particular early-pause placement.
- [ESP-MQTT programming guide](https://docs.espressif.com/projects/esp-mqtt/en/latest/esp32/):
  TLS client support and automatic QoS 1/2 retransmission behavior.
- Existing [DS18B20 design](2026-11-01-ds18b20-sensor-design.md),
  [Wi-Fi design](2026-10-25-wifi-fallback-design.md), and
  [OTA design](2026-10-25-github-ota-design.md).
