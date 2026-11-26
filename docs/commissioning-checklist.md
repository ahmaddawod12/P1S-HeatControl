# P1S Heat Controller Commissioning Checklist

For current Firebase firmware, use the signed-in hosted dashboard for live
readings and supported commands. Verify that `192.168.4.1` serves Wi-Fi setup
while disconnected and pairing while connected and unpaired. Confirm the
hotspot closes after pairing and stays off during a Firebase outage with
Wi-Fi still connected. LAN dashboard, REST/SSE and dashboard OTA checks below
preserve results for earlier firmware.
[Firebase bench verification](../cloud/README.md#deploy-to-a-firebase-test-project)

Printer integration status (2026-11-08): **Monitor P1S** replaces Preheat &
Resume (see the [Monitor P1S design](superpowers/specs/2026-11-07-monitor-p1s-design.md)).
The Firebase bench below records live monitoring with the relay contacts
unloaded. Heated trials remain pending.
Earlier monitoring observations are recorded in
[printer qualification](printer-integration-qualification.md). Complete the
original relay/sensor stages before heated testing.

## Optional printer integration — pending

- [ ] Record actual P1S firmware, Bambu Studio version and explicit board/port authorization. The printer stays in normal cloud mode; no Developer Mode or time server.
- [ ] Prove verified TLS monitoring, the reported `printer_state`, and a steady `reconnects` count while Bambu Studio sends a file to the printer.
- [ ] With relay loads disconnected, verify that a leftover FINISH waits for the next print, PAUSE holds the heater and fan off, RUNNING allows heating, and FINISH or FAILED ends a watched session.
- [ ] Prove connection loss holds the heater off and recovery brings it back; prove the 60-minute no-print timeout.
- [ ] Prove Disable, a fault, maintenance and a settings change end monitoring, and that pressing Monitor P1S again keeps heating.
- [ ] Run the native Unity/API cases on an explicitly selected authorized board, including four SSE clients.
- [ ] Measure heap/largest-block and task-stack margins with TLS, maximum reports, SSE/polling and idle OTA suspension.
- [ ] Power-cycle the controller with the loads disconnected. A session waiting for a print may resume once valid sensor samples and a fresh printer report arrive. A session already watching a print ends at reboot. Verify that the relay stays off even when the first fresh report shows RUNNING, including when a different print started during the outage.
- [ ] Power-cycle again after disabling heating. Verify that the relay stays off and the saved printer reconnects.
- [ ] After unloaded-relay tests pass, supervise a heated print with Monitor P1S through the end of the print and confirm the heater and fan switch off together.

Use the detailed evidence matrix in
[printer integration qualification](printer-integration-qualification.md).

### Firebase USB bench, 2026-11-26

Tested one ESP32 on COM3 against the hosted Firebase pilot and a P1S running
firmware `01.10.00.00` in cloud mode. The heater and fan were disconnected
from the relay contacts throughout.

| Check | Observed result |
|---|---|
| Controller identity and connection | The setup hotspot suffix matched the board ID shown on the dashboard. Wi-Fi and the existing enrollment survived each reset. |
| Dashboard commands | The controller acknowledged the lockout, applied changes to the target, restart threshold and sensor calibration, and reported both outputs on and off with Enable and Disable. Final settings were 60 °C target, 59 °C restart and 0 °C calibration offset. |
| Printer link and monitoring | The controller read the active print from the saved LAN connection. All 13 samples over about five minutes included a valid printer state and a ready command connection. Start Monitor P1S showed Monitoring. Stop monitoring left heating enabled until Disable was applied. |
| Reset during an active print | The controller ended monitoring and reported restart lockout with both outputs off even after reading the printer's Printing state again. Acknowledging the fault left the outputs off. |
| Reset with heating disabled | The controller returned to idle with both outputs off, retained its settings, and reconnected to Firebase and the printer. Transient TLS handshakes failed during startup and recovered on retry. |
| Native tests and probe | All 67 on-device Unity cases passed. The physical DS18B20 returned 10 valid samples averaging 27.12 °C. |

### Subscription-only printer and memory bench, 2026-11-26

The same ESP32 and P1S were used with both relay loads disconnected. The
controller subscribed to regular printer reports and kept the run state for
Monitor P1S. After reconnect, it waited for a report containing `gcode_state`.

| Check | Observed result |
|---|---|
| RAM at idle with Wi-Fi, Firebase and printer connected | Median sampled internal heap increased from 44.3 KiB to 78.8 KiB. The largest free block at the end of the baseline increased from 31 KiB to 62 KiB. |
| Forced memory pressure | Firebase TLS allocations failed at both pressure levels, starting at 24,832 and 13,664 free bytes. The recorded heap low-water mark reached 324 bytes. The controller stayed running with both outputs off, and Firebase recovered after each reservation was released. |
| Wi-Fi reconnect recovery | All eight cycles restored the Firebase command stream and printer subscription in 8.04–8.11 seconds. A later regular report supplied the live Printing state. |
| Seven-minute soak | The repeat recorded 420.4 seconds, zero unexpected resets, and zero allocation failures. Wi-Fi and printer reports continued after startup. Firebase commands became available at 61 seconds and stayed available through the end. |
| Temperature probe | The DS18B20 data line read low during the repeat. Reseating the connector restored valid 26.75 °C readings, which continued through the end. Both outputs remained off throughout the test. |
| Final pilot firmware | The Firebase time-sync budget now includes the SDK's configured SNTP startup delay. All 140 host tests passed, including a clock reply that arrives after the old budget. On the board, fresh telemetry with Firebase commands and the printer connected was verified within 32.3 seconds of reset. The probe read 26.75 °C, both outputs were off, and the 60 °C target, 59 °C restart and 0 °C calibration offset were retained. |

### Installed pilot observation, 2026-11-26

The hosted dashboard reported **Applied** for **Acknowledge fault**. The
controller returned to Idle with heating disabled and both outputs off.

On the installed pilot firmware, the UART observer ran for 606.8 seconds and
recorded zero resets, fatal errors or Wi-Fi disconnects. All 41 Firebase
samples were fresh with commands ready, the probe valid, the P1S connected
and reporting Printing, and both outputs off. The oldest observed telemetry
was 16.62 seconds. The target, restart temperature and calibration offset
remained at 60 °C, 59 °C and 0 °C.

All 22 cloud tests and 13 browser tests passed, including owner isolation
and independent controls for two boards under the same owner. The production
dashboard build passed. This hardware observation covered one board, with both
relay loads disconnected.

### Live RAM reporting, 2026-11-26

The pilot dashboard shows free internal heap and the heap low-water mark in
the Firmware panel. The controller sends both values with its existing status
updates. The ESP-IDF low-water mark sums the minimum free space in each heap
region since boot.

| Mode | Samples | Free RAM |
|---|---:|---:|
| Idle before the test | 3 | 81.4–81.7 KiB |
| Heating enabled | 2 | 80.7–80.8 KiB |
| Heating enabled with Monitor P1S watching an active print | 3 | 80.6–81.8 KiB |

The heater and fan were physically disconnected from the relay contacts.
The board reported both outputs on in the two active test modes. The heap
low-water mark held at 30.3 KiB and UART captured zero unexpected resets or
fatal errors. Disable returned the board to Idle with both outputs off and
Monitor P1S off. The last sample showed 81.8 KiB free.

### Printer handshake recovery, 2026-11-26

The controller retries a printer connection if MQTT connection confirmation
takes 30 seconds or the report subscription confirmation takes 15 seconds.
A rejected subscription also triggers a retry. Repeated failures back off
until a subscription succeeds. Monitor P1S still waits for a regular report
containing the print state after a reconnect.

Three host cases reproduced the stalled or rejected connections and passed
with the fix. The full 143-case host suite passed. The updated pilot booted,
reconnected to Firebase and subscribed to the P1S with both outputs off.
A regular printer report then restored the Printing state.

Work through the stages **in order**. Record a pass or fail with the date and
your initials for every line. A stage that fails stops commissioning — fix the
cause and restart that stage before moving on.

The heater uses 220 VAC. Perform stages 1 through 5 with the relay contacts
unloaded. Heater integration begins in stage 6.

## Recorded configuration

Fill this in before starting. Repeat sensor and heater commissioning after the
DS18B20 change; earlier NTC results below are historical, not DS18B20 validation.

| Item | Value |
|---|---|
| Firmware commit | |
| Firmware tag | |
| ESP-IDF version | 6.1 |
| Board / target | ESP32 Dev Kit / `esp32` |
| Board ID from setup hotspot | |
| Flash / console | 4 MB / UART0 through USB-to-UART |
| DS18B20 DATA input | GPIO27 |
| DS18B20 supply / ground | 3.3 V / ESP32 GND |
| Probe lead colors | Yellow DATA, red VCC, black GND |
| Cable maximum temperature | 85 °C; route away from the heater |
| Heater and fan relay output | GPIO16 |
| Relay polarity (`CONFIG_P1S_RELAY_ACTIVE_LOW`) | |
| Wi-Fi SSID (`CONFIG_P1S_WIFI_SSID`) | |
| DS18B20 calibration offset (°C) | Starts at 0 after NTC migration |
| DATA pull-up to 3.3 V measured (Ω) | Verify adapter resistor; add 4.7 kΩ if absent |
| Old NTC divider / 100 nF capacitor | Disconnected from the new DATA circuit |
| Fan label current (A) | |
| 5 V supply rating (A) | |
| Reference thermometer make/model | |
| Commissioned by | |
| Date started | |

---

## Stage 1 — First power-up, contacts unloaded

Start with the relay contacts unloaded and heating disabled.

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 1.1 | Relay contacts confirmed unloaded before power | Verified by inspection | | |
| 1.2 | Board powers up, serial log reaches `controller services started` | No brownout or reset loop | | |
| 1.3 | Reported state after boot | `boot` then `idle` | | |
| 1.4 | Reported enabled state after boot | `false` | | |
| 1.5 | Unpaired controller joins the recorded SSID | Serial log shows the station address. The setup page switches to the pairing code. | | |
| 1.6 | Complete pairing on the hosted dashboard | The board's setup hotspot shuts down and the dashboard shows its board ID and live readings. | | |

## Stage 2 — Relay indicators through boot and reset

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 2.1 | Relay indicator during power-on with heating disabled | Off the entire time | | |
| 2.2 | Relay indicator during EN/reset press | Off, no pulse or flicker | | |
| 2.3 | Relay indicator after firmware flash | Off, no pulse | | |
| 2.4 | Repeat 2.2 five times | Off every time | | |

Record any observed pulse width here, however brief: __________________

## Stage 3 — Ambient DS18B20 comparison

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 3.1 | Reference thermometer reading (°C) | Recorded | | |
| 3.2 | Dashboard filtered reading (°C) | Recorded | | |
| 3.3 | Difference | Within ±2 °C before calibration | | |
| 3.4 | Calibration offset applied (°C) | Recorded, within ±10 °C | | |
| 3.5 | Reading after calibration | Within ±0.5 °C of reference | | |
| 3.6 | Reading stability over 5 minutes | Drift under 0.5 °C | | |

## Stage 4 — DS18B20 fault injection

Perform with contacts still unloaded. Only short
DATA to GND for step 4.4; do not short the 3.3 V supply.

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 4.1 | Disconnect the probe DATA lead while heating | First four 1-second failed reads retain heater output; fifth latches `sensor_open` | | |
| 4.2 | Output after 4.1 latches | Shared relay off | | |
| 4.3 | Reconnect lead, acknowledge | Refused until three clean samples, then `idle` | | |
| 4.4 | Short GPIO27 DATA to GND while heating | First four failed reads retain output; fifth latches `sensor_short` | | |
| 4.5 | Output after 4.4 latches | Shared relay off | | |
| 4.6 | Restore, acknowledge | Returns to `idle`, still disabled | | |
| 4.7 | Enable attempt while faulted | Refused with HTTP 409 | | |
| 4.8 | Boot with the probe disconnected | Dashboard available; fifth failed read latches `sensor_open`; heater and fan remain off | | |
| 4.9 | Reconnect after 4.8, wait for three clean samples and acknowledge | Returns to `idle`; remains disabled | | |
| 4.10 | Inject four failed reads, then one successful read, then four failures | Failure streak resets on success; heating continues without a latched fault | | |
| 4.11 | Fault kind alternates with no successful read | Fifth failed read still latches the current sensor fault | | |
| 4.12 | Stop acquisition after one failed read | Stale-task fault still stops heating after 2 s without a completed read | | |

## Stage 5 — Shared relay control

Keep the relay contacts unloaded and observe the indicator and dashboard.

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 5.1 | 5 V supply rating vs fan label current | Supply rating exceeds fan draw | | |
| 5.2 | Enable with target above ambient | GPIO16 relay energises and the dashboard shows Heater and fan on | | |
| 5.3 | Disable | Relay de-energises and the dashboard shows Heater and fan off | | |

## Two-board Firebase pilot

Run these checks alongside stages 1 through 5 with both relay contacts
unloaded. Pair both boards with one approved test account.

| Check | Expected | Result / date |
|---|---|---|
| Power both boards before pairing | The hotspots have distinct suffixes. Each setup page shows the matching board ID. | |
| Finish pairing the second board while viewing the first | The first board remains selected. Both boards appear with matching IDs and separate live readings. | |
| Select a board and change its target. Select the other and use Disable heat. | Each command result belongs to the selected board. The target changes only on the first board and Disable switches off only the second relay. | |
| Power-cycle one board | The other board stays online and responds to its own commands. The restarting board keeps its ID and returns to its own dashboard entry. | |

## Stage 6 — Heater integration

**Only after stages 1 through 5 all record a pass.**

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 6.1 | Stages 1–5 all passed | Confirmed | | |
| 6.2 | Mains wiring inspected, earth continuity checked | Verified | | |
| 6.3 | Loads connected, target 30 °C, enable | The heater and fan turn on together | | |
| 6.4 | Chamber temperature rises | Reading tracks upward | | |
| 6.5 | Emergency disable responds | Heater and fan switch off together | | |

## Stage 7 — Regulation at 30, 45, and 60 °C

Let each setpoint settle before recording. Note the peak temperature reached
after the output first switches off.

| Target | Time to reach (min) | Peak overshoot (°C) | Steady-state band (°C) | Result | Date / initials |
|---|---|---|---|---|---|
| 30 °C | | | | | |
| 45 °C | | | | | |
| 60 °C | | | | | |

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 7.1 | Output switches off at the target | Confirmed at all three setpoints | | |
| 7.2 | Output switches back on at the configured restart temperature (default `target - 1`) | Confirmed at all three setpoints | | |
| 7.3 | No relay chatter | Minimum 5 s between normal transitions | | |
| 7.4 | Overshoot recorded above | Within acceptable range for the chamber | | |
| 7.5 | Target is reached after active heating | Heater and fan switch off together | | |
| 7.6 | Set target to 0 while heating | Heater and fan switch off together | | |

## Stage 8 — Wi-Fi outage during regulation

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 8.1 | Disable the access point while `heating` | Regulation continues unchanged | | |
| 8.2 | Relay state during 8.1 | Unchanged by the outage | | |
| 8.3 | Serial log during 8.1 | Reconnect attempts at 1, 2, 4, 8, 30 s | | |
| 8.4 | Restore the access point | Reconnects, dashboard resumes | | |
| 8.5 | Repeat 8.1 while `holding` | Regulation continues unchanged | | |
| 8.6 | Dashboard during outage | Shows disconnected, keeps last measurement | | |
| 8.7 | Setup hotspot while Wi-Fi is unavailable | `P1S-Heat-Setup-<board ID>` serves Wi-Fi setup at `192.168.4.1`. | | |

## Wi-Fi settings commissioning — 1.0.5, loads disconnected

| # | Check | Observed / remaining |
|---|---|---|
| W.1 | Generic startup without saved credentials | AP enabled at 925 ms; HTTP at 955 ms; no panic during 12-second serial probe |
| W.2 | Saved network unavailable for 30 seconds | Exact boundary and repeated failures pass host tests; actual RF/client timing pending |
| W.3 | Visible fields, exact spaces and persistent replacement | Browser and native storage/HTTP cases pass; live hotspot client Save pending |
| W.4 | Wrong password correction, same page and no reboot | PC AP association canceled; original network/configuration restored. Owner takes over manual client test |
| W.5 | Station recovery and hotspot shutdown | Same-network Save kept STA connected and closed AP at 10.66 s without reboot; recovery after a wrong password still pending |
| W.6 | Wi-Fi versus OTA/trial admission | Real atomic race tests and native firmware admission pass |
| W.7 | Board restoration after native/startup tests | Signed confirmed 1.0.4 and original configuration restored, flash hash verified; original Wi-Fi, target 60 °C, fault latched and both outputs off read back |
| W.8 | Signed 1.0.5 installation through OTA | Approved public assets anonymously verified; 1.0.4 → 1.0.5 installed and confirmed in 38.61 s; saved Wi-Fi/target retained, outputs off |

## Stage 9 — Power interruption during manual heating

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 9.1 | Cut power while `heating` | Both channels de-energise | | |
| 9.2 | Restore power | Both channels start de-energised | | |
| 9.3 | Restore power after an interrupted heat session | The dashboard shows a restart lockout. Heating stays disabled after Firebase sync | | |
| 9.4 | Relay indicators through the restart | Off for at least five seconds after the relay is confirmed off | | |
| 9.5 | Target and restart temperature after reboot | Match the persisted values | | |
| 9.6 | Wait for three valid samples and readings below 80 °C, then acknowledge the lockout | Heating stays disabled until Enable heat is selected. Normal regulation then uses the saved target and restart temperature | | |
| 9.7 | Reboot with a pending Disable, or restore without saved enrollment | Heating stays disabled | | |

## Stage 10 — Maximum target

**Only after stage 7 records stable results at all three lower setpoints.**

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 10.1 | Stage 7 stable at 30, 45, and 60 °C | Confirmed | | |
| 10.2 | Target 80 °C, enable | Regulates to setpoint | | |
| 10.3 | Peak overshoot at 80 °C (°C) | Recorded, stays below 82 | | |
| 10.4 | Over-temperature fault not triggered | No latched `overtemperature` | | |
| 10.5 | Dashboard warning above 60 °C | Visible and persistent | | |

## OTA commissioning history — loads disconnected

Run this section after the software/low-voltage checks and before accepting OTA
for an enclosed printer. See [the release guide](ota-release-guide.md) for the
personal signed bootstrap and generic release procedure. These checks are in
progress; consult the implementation status for measured results.

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| O.1 | Install signed `1.0.0` USB bootstrap | Both GPIOs inactive throughout; local trial confirms within 120 s | Signed USB write/hash and local health passed; GPIO levels not probed | 2026-10-25 / Codex |
| O.2 | Record target/calibration; power cycle | Saved values and Wi-Fi retained; heating disabled | Passed: 55.5 / −1.25 and exact Wi-Fi read back after all OTA/fault tests; original 60 / 0 restored and read back | 2026-10-25 / Codex |
| O.3 | Publish compatible signed `1.0.3`; press Update once | One attempt; heater off promptly; fan cooldown completes before writing | 1.0.0 → 1.0.3 confirmed; duplicate 409; cooldown verified by runtime tests, live board already inhibited by sensor fault | 2026-10-25 / Codex |
| O.4 | Try enable, target save, manual fan and fault acknowledge during maintenance | Rejected; no saved target change; Disable remains available | Passed: blocked controls 409, Disable 204, target unchanged | 2026-10-25 / Codex |
| O.5 | Observe flash, reboot and trial with relay indicators/logic probe | No heater pulse; correct version after confirmation; heating remains disabled | Correct version/API disabled outputs passed; owner saw indicators off during power cut/restart; full GPIO trace pending | 2026-10-25 / Codex + owner |
| O.6 | Install signed `1.0.4` | Other slot direction passes; credentials/target/calibration retained | Passed: 1.0.3 at 0x210000 → 1.0.4 at 0x20000; both SDK slots VALID; settings/Wi-Fi retained | 2026-10-25 / Codex |
| O.7 | Interrupt Wi-Fi and power during a download | Previous confirmed image boots; clear failure/interrupted result; no automatic retry | USB power cut and RTS reset passed; deliberate Wi-Fi interruption pending | 2026-10-25 / Codex + owner |
| O.8 | Try altered, unsigned, wrong-key or incompatible controlled candidate | Rejected before activation; previous image remains selected | Host/package/transfer checks pass; physical flash rejection pending | |
| O.9 | Controlled candidate fails trial HTTP/runtime health or stalls startup | Watchdog/deadline causes rollback; dashboard returns; no repeat loop | Passed: unconfirmed reset, 22.6 s startup watchdog and 122.8 s missing-HTTP deadline all returned to 1.0.3; dashboard restored | 2026-10-25 / Codex |
| O.10 | Existing sensor fault and four SSE clients during trial | Safe responsive local service remains recoverable | Passed: four real streams observed while 1.0.4 was pending, then confirmed; sensor fault latched, outputs disabled | 2026-10-25 / Codex |
| O.11 | Router outage after a confirmed update | Normal regulation continues; no rollback | Host policy passes; physical regulation/outage test pending | |
| O.12 | Lost response/stale dashboard around accepted update | No automatic POST retry or duplicate worker; version/rollback shown accurately | Passed: one worker, reboot version reload, interruption and rollback messages, no automatic retry | 2026-10-25 / Codex |
| O.13 | Restore normal signed build after all fault injection | No deliberate faults or personal credentials in public assets | Passed: final public 1.0.4 installed by real OTA; no local fault hook or setup credentials in the published application | 2026-10-25 / Codex |

## Sign-off

| Item | Value |
|---|---|
| All stages passed | No; OTA core checks above passed, remaining physical commissioning rows stay open |
| Outstanding issues | Sensor input reports short/over-temperature faults; GPIO trace, deliberate Wi-Fi interruption/regulation outage and physical bad-image rejection remain open |
| Commissioned by | |
| Date completed | |

Tag the release only after this page is complete.
