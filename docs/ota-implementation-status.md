# OTA implementation and board test status

**Historical record:** This document describes the earlier commissioning builds
and separate distribution repository. The 2026-11-07 public source release
resets the version to 1.0.0, uses active-high relays and publishes from
`ahmaddawod12/P1S-HeatControl`. Follow the
[current release guide](ota-release-guide.md). Earlier physical results below
do not establish commissioning of the new baseline.

Implemented on `feature/p1s-heat-controller`. The owner resumed hardware testing
and confirmed both relay loads disconnected. The ESP32-D0WD-V3 / 4 MB board is
connected on COM3 at `10.0.0.31`. Signed USB setup, a live GitHub OTA installation,
an interrupted download, a physical USB power cut, rollback after an unconfirmed
boot and watchdog recovery from stalled startup have passed.
The clean public 1.0.4 image is now confirmed in the other slot, and original
settings have been restored and read back. No eFuse settings have been changed.

## Software verification

- CMake/CTest: 13 of 13 host suites pass. These include the real runtime tasks
  with threaded external boundaries, maintenance/save races, cooldown and failed
  output acknowledgement; manifest, transfer, update job, trial decision and
  origin/body policies; device configuration storage and existing control logic.
- Node dashboard behavior: 5 of 5 tests pass, including uncertain POST responses
  and reboot/rollback recovery. A local browser fixture also showed download
  progress and disabled heating controls with Disable available.
- Python packaging: 10 of 10 tests pass, using real temporary-key signatures and
  wrong-key/tamper/unsigned/personal-configuration/layout rejection, including
  same-version personal-image substitution and build-payload mismatches.
- Publisher boundaries: missing, empty, initialized and private repository
  scenarios pass with a fake GitHub CLI. Public test publication and anonymous
  asset verification have also passed against the real distribution repository.
- ESP-IDF v6.1 generic application, personal bootstrap and native Unity test
  image all build successfully. The complete on-board Unity suite passes
  **89/89**, with zero failures, errors or skipped cases.
- Signed generic package validation, publication dry run, and personal bootstrap
  signing/validation/preparation all pass. Application size is 1,179,636 bytes
  within a 2,031,616-byte slot; bootloader size is 26,272 within 28,672 bytes.

The corrected generic bundles are under the ignored `dist/firmware-1.0.3/` and
`dist/firmware-1.0.4/`. Each bundle's only
files are `firmware.json` and `p1s-heat-controller-esp32-4mb.bin`. The signed
personal image is in the ignored `build-bootstrap/`; it contains Wi-Fi credentials
and must stay private. The private signing key is outside the checkout as
documented in the [release guide](ota-release-guide.md).

The approved public repository and test releases now exist at
`ahmaddawod12/P1S-HeatControl-Releases`. Version 1.0.1 is retained as a prerelease
with a known-issue note after board tests exposed IPv4-mapped origin handling.
Version 1.0.2 is also retained as a prerelease: its HTTP request buffer could not
hold GitHub's signed asset redirect URL. Corrected 1.0.3 and 1.0.4 were separately
approved and published for commissioning. Both assets of each were downloaded
anonymously and their real signature/digest verified. Version 1.0.4 is the latest
stable test release and the board's final confirmed image. Version 1.0.3 remains
the previous confirmed image. Both test releases are labeled for commissioning;
their compiled assets were not changed after publication.

## Live board evidence

| Check | Observed result |
|---|---|
| USB bootstrap | Signed 1.0.0, complete write hash verified at 115200; explicit first-boot health marker saved |
| Network | Personal bootstrap joined the PC's 2.4 GHz network; generic firmware rejoined using saved credentials |
| HTTP boundaries | Wrong Origin/token rejected with 403; invalid body 400; four SSE clients plus status/firmware GET remained responsive; fifth SSE rejected with 503 |
| First OTA | 1.0.0 at `0x20000` → 1.0.3 at `0x210000`; 1,179,636 bytes received, verified, activated and locally confirmed |
| Maintenance | Duplicate update, enable, manual fan, fault acknowledge and target change rejected with 409; Disable accepted with 204; target remained 55.5 °C |
| Up-to-date check | An explicit second dashboard check returned `up_to_date` without entering maintenance |
| Incomplete download | RTS reset at 147,456 / 1,179,636 bytes; confirmed 1.0.3 booted, result `interrupted`, dashboard recovered |
| Unconfirmed boot | 1.0.4 started at `0x20000`, reset before application health confirmation; bootloader returned to 1.0.3 at `0x210000`, result `rolled_back` |
| Physical power cut | Last pre-cut API sample showed 716,800 / 1,179,636 bytes; owner unplugged USB, waited five seconds and reconnected; 1.0.3 returned with `interrupted` and target 55.5 °C |
| Relay observation | Owner confirmed both relay indicators stayed off through that USB power cut and restart |
| Startup watchdog | Private signed local fixture stalled before runtime startup; `ota_trial` watchdog fired at 22,639 ms; bootloader returned to 1.0.3, result `rolled_back`, settings and disabled outputs retained |
| HTTP health deadline | Private signed fixture joined Wi-Fi but omitted HTTP startup; rollback occurred at 122,763 ms without a watchdog panic; 1.0.3 and its dashboard returned |
| Second OTA | Clean public 1.0.3 → 1.0.4 passed in 76.4 seconds; four real SSE clients were observed during the pending trial and after confirmation |
| Final flash state | 1.0.4 boots at `0x20000`; OTA sequences 3 and 2 both have SDK state VALID; saved attempt 7 records installed 1.0.4 |
| Retention and restoration | After all updates/fault tests, flash readback retained 55.5 °C / −1.25 °C and exact saved Wi-Fi; original NVS restored with write hash verification, then read back as schema 1 / 60 °C / 0 °C with Wi-Fi and both valid slots unchanged |
| Retry behavior | Both recovery cases remained on the old image without an automatic retry for at least 10 seconds |
| Safety state | API reported disabled heater/fan throughout sampled maintenance and recovery; sensor faults stayed latched |

RTS reset cases and the owner-assisted USB power cut are separate evidence.
Relay indicator observation covers that power cut/restart; GPIO voltages have
not been probed. Deliberate Wi-Fi loss, altered-image rejection on real flash
and regulation through a router outage remain unverified on the board.
Host/native policy and cryptographic tests cover their software boundaries.

The sensor has reported `sensor_short` and `overtemperature`, with sharply
changing readings and intermittent valid samples while heating was disabled.
Fresh fault samples and safe outputs permit OTA recovery as designed; this does
not confirm working temperature measurement or heater commissioning. Keep the
fault latched until its cause is resolved.

The USB link failed during full-flash reads at high baud. The original NVS
partition was successfully read and verified at 115200, with original target
60 °C and calibration 0 °C. It is saved privately under
`build-bootstrap/private-backups/`. Testing used a temporary target
55.5 °C and calibration −1.25 °C to prove OTA retention. The original NVS has now
been restored and read back with target 60 °C and calibration 0 °C. USB bootstrap
writing at 115200 passed its complete
flash hash verification. The owner authorized replacing local bootstrap setup
credentials with the active PC network's exact SSID and key; only ignored local
setup/build files contain the key. The generic public images contain no setup
credentials.

The original staged ESP32 migration was preserved byte-for-byte. OTA edits on
those same files intentionally remain unstaged, including SDK defaults, Wi-Fi
startup, web route registration, native HTTP tests, README and commissioning
checklist. Review and retain the complete working tree when resuming; the OTA
commits alone do not include these edits.

## Decisions and their limits

These are the execution ledger's rulings, in order. Deferrals in 6, 10 and 11
were partly lifted when the owner resumed board testing; current evidence is
listed above.

1. Reuse the existing feature checkout following “Implement here,” retaining the
   staged hardware baseline. Isolation relies on the current feature branch.
2. Preserve the owner's index exactly and commit other paths separately. Some
   reviewed OTA changes consequently remain unstaged and need later integration.
3. Use a fresh ignored host build directory because the installed tools moved.
   Reusing an old cache would make local verification unreliable.
4. Exercise the actual transfer algorithm through external flash/network/hash
   boundaries on the host. This adds an internal interface and does not prove
   real ESP32 flash or network behavior.
5. Let a target save admitted before maintenance settle its queued target, while
   rejecting new saves and unsafe commands. This keeps stored/reported targets
   consistent; an indefinitely blocked storage call still needs diagnosis.
6. Run host health/runtime decisions and compile native adapters, retaining
   native watchdog/socket/boot recovery execution for the deferred board tests.
   Physical recovery is not established by these software checks.
7. Make scripts prepare/verify by default, with explicit `-Flash`/`-Publish` for
   final writes. The operator still needs to execute those deployment steps.
8. Commit transport, worker and trial integration together because their source
   list and startup dependencies are shared. Per-task commit isolation is reduced.
9. Extend the signed descriptor to format 2 with 116 bytes before the first
   release, binding purpose/credential flags to the compiled image. Earlier
   prepared format-1 images are replaced; none were deployed or published.
10. Honor the reviewer's deferral of real hardware behavior to the owner-approved
    later session. Relay and recovery results remain unverified.
11. Retain the reviewer's actual publication/permissions deferral. Anonymous
    public download access still needs verification after publication.
12. Keep the approved exclusions for physical secret extraction, hardware secure
    boot, LAN accounts and runtime provisioning. Physical flash access can
    expose secrets, and the local service trusts the LAN.
13. Retain existing stale-sample and ordinary settings-corruption policies outside
    the OTA change. Those pre-existing behaviors remain a separate concern.
14. Keep this author-checked handoff document added after the independent review
    range. It records evidence but was not independently re-reviewed.
15. Reset into serial download mode before each separate esptool invocation;
    the previous high-baud stub did not reconnect at the next process's initial
    baud. Intermediate commands stay in download mode; USB recovery remains
    necessary if a transfer is interrupted.
16. Keep the immutable 1.0.1 test assets as a prerelease after native failure and
    prepare fixed 1.0.2. Subsequent live testing also exposed its transport buffer
    limit; additional approved versions are needed for both slot directions.
17. Require a dedicated bootstrap confirmation marker because IDF marks the
    empty initial slot valid before local health checks. This adds one versioned
    8-byte NVS record and a brief early maintenance gate on personal-image boots.
18. Increase the bounded HTTP transmit buffer to 4096 bytes after the SDK's
    512-byte default rejected GitHub's signed redirect. Retain 1.0.2 as a
    prerelease and use approved 1.0.3/1.0.4. This uses extra heap per connection
    and adds two test releases; the native regression now passes end to end.
19. Use signed private USB fixtures to exercise startup stall and missing HTTP
    health, preserving the confirmed slot and preparing a checked NEW entry for
    the inactive slot. Fixtures commit the same activated-attempt shape as the
    real updater. Incorrect test metadata would require USB recovery; neither
    fixture nor its build hook is included in public firmware or tracked code.

## Independent review

A fresh `gpt-6-astra` reviewer inspected the implementation and unstaged
integration. It identified three Important issues and no Critical or Minor
issues: personal-image substitution at publication, maintenance losing a
latched fault, and release-tag creation in an empty repository. Each received a
reproducing failing test and a fix. Maintenance now preserves recovered sensor,
NVS and output-init faults until their normal acknowledgement policy allows
clearing; public image purpose and build binding are checked; new/empty release
repositories receive a source-free initial README commit. No minors were deferred.

On-board testing then exposed two additional issues missed by host-only checks:
the new HTTP test's local buffers exceeded the Unity main-task stack, and
ESP-IDF's IPv6 listener returned mapped IPv4 socket addresses. Static fixture
buffers fixed the test stack; native origin validation now extracts mapped IPv4
without relaxing Host, Origin or token checks. The existing on-board POST test
failed with 403 before the fix and passes afterward, as does the complete suite.

Physical OTA metadata readback also showed that the SDK initializes the empty
first slot as valid before application health runs. The personal bootstrap now
requires a separate committed `boot_ok_v1` marker after full local health. A
previously confirmed personal image recognizes that marker without requiring
the router. The previous VALID-only decision failed a host regression; marker
storage/persistence/future-format checks and the expanded board suite pass.

The first real GitHub preflight then validated TLS certificates but failed
before flashing with `HTTP_CLIENT: Out of buffer`. The SDK's default 512-byte
request buffer could not construct the signed redirect request. Increasing its
bounded transmit buffer to 4096 bytes changed that actual failing check into a
complete, verified 1.0.3 installation. Generic and personal builds both pass.

An additional existing cosmetic issue was observed: the SSE broadcaster clears
uptime for change comparison and serializes that value into the event, so the
dashboard can show zero uptime. `/api/status` reports advancing uptime correctly.
This is deferred outside OTA; it does not affect control or update decisions.

## Resume

The board is left on confirmed public 1.0.4, connected to the PC network at
`http://10.0.0.31/`, with original 60 °C / 0 °C settings and heating disabled.
The final live status reports `sensor_short`.

Follow the [release guide](ota-release-guide.md) and the remaining OTA rows of
the [commissioning checklist](commissioning-checklist.md). Resolve the sensor
input fault before heat/regulation testing. Deliberate Wi-Fi interruption,
router-outage regulation, a full GPIO trace and physical bad-image rejection
remain open; do not mark the whole printer commissioning complete.

During the 2026-11-01 repository cleanup, private readbacks, original settings
backups, test evidence, and existing release packages were archived locally
outside the checkout before generated build/test directories were removed.
Keep that local archive for outstanding commissioning checks. Private fault
fixtures are named `LOCAL_*`; no test hook is present in tracked product code
or public 1.0.4. Regenerate `build-release/` for normal public packaging.
Keep personal builds/backups and the private signing key private. Source is
public under the current release policy.
