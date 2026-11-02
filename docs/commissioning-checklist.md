# P1S Heat Controller Commissioning Checklist

Printer integration status (2026-11-02): source tests/builds are recorded in
[printer qualification](printer-integration-qualification.md). No printer
connection, board flashing, native Unity execution or heated integration trial
was performed for this feature. Automatic Resume remains disabled in generic
builds. Complete the original relay/sensor stages below before heated testing.

## Optional printer integration — pending

- [ ] Record actual P1S firmware, Bambu Studio version, LAN/developer mode and explicit board/port authorization.
- [ ] Prove verified status-only TLS, usable run identity, report cadence and repeated identical filenames/local `0` IDs.
- [ ] Qualify the PC startup pause before purge, prime and model extrusion; record earlier motion/heating.
- [ ] Prove one live QoS 0 Resume, no queued/reconnected replay, and the native 1,000 ms send deadline under backpressure.
- [ ] Prove Stop/Disable/fault/lease expiry during a waiting send and hold the operation owner until outputs acknowledge teardown.
- [ ] Run the native fake-transport Unity/API cases on an explicitly selected authorized board, including four SSE clients.
- [ ] Measure heap/largest-block and task-stack margins with TLS, maximum reports, SSE/polling and idle OTA suspension.
- [ ] After unloaded-relay passes, supervise preheat → one Resume → hold → real finish → heater off/cooldown, plus each interruption case.
- [ ] Verify reboot/reconnect never restores heat or Resume; recheck manual heating, manual fan and thermal faults.
- [ ] Record the exact qualified profile/version/build combination before enabling an opted-in commissioning image.

All boxes above remain unchecked. Use the detailed evidence matrix in
[printer integration qualification](printer-integration-qualification.md).

Work through the stages **in order**. Record a pass or fail with the date and
your initials for every line. A stage that fails stops commissioning — fix the
cause and restart that stage before moving on.

The heater channel switches 220 VAC. Stages 1 through 5 are performed with both
relay contacts unloaded; the heater is not connected until stage 6.

## Recorded configuration

Fill this in before starting. Repeat sensor and heater commissioning after the
DS18B20 change; earlier NTC results below are historical, not DS18B20 validation.

| Item | Value |
|---|---|
| Firmware commit | |
| Firmware tag | |
| ESP-IDF version | 6.1 |
| Board / target | ESP32 Dev Kit / `esp32` |
| Flash / console | 4 MB / UART0 through USB-to-UART |
| DS18B20 DATA input | GPIO27 |
| DS18B20 supply / ground | 3.3 V / ESP32 GND |
| Probe lead colors | Yellow DATA, red VCC, black GND |
| Cable maximum temperature | 85 °C; route away from the heater |
| Heater relay output | GPIO25 |
| Fan relay output | GPIO26 |
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

Both relay COM/NO contacts disconnected. No heater, no fan wired through.

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 1.1 | Both relay contacts confirmed disconnected before power | Verified by inspection | | |
| 1.2 | Board powers up, serial log reaches `controller ready and disabled` | No brownout, no reset loop | | |
| 1.3 | Reported state after boot | `boot` then `idle` | | |
| 1.4 | Reported enabled state after boot | `false` | | |
| 1.5 | Joins the recorded SSID | Serial log shows the station address | | |
| 1.6 | Setup hotspot after normal station connection | `P1S-Heat-Setup` absent after its 10-second grace period | | |

## Stage 2 — Relay indicators through boot and reset

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 2.1 | Both relay indicators during power-on | Off the entire time | | |
| 2.2 | Both relay indicators during EN/reset press | Off, no pulse or flicker | | |
| 2.3 | Both relay indicators after firmware flash | Off, no pulse | | |
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

Perform with contacts still unloaded and manual fan override off. Only short
DATA to GND for step 4.4; do not short the 3.3 V supply.

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 4.1 | Disconnect the probe DATA lead | State `fault`, fault `sensor_open` | | |
| 4.2 | Outputs during 4.1 | Both off | | |
| 4.3 | Reconnect lead, acknowledge | Refused until three clean samples, then `idle` | | |
| 4.4 | Short GPIO27 DATA to GND | State `fault`, fault `sensor_short` | | |
| 4.5 | Outputs during 4.4 | Both off | | |
| 4.6 | Restore, acknowledge | Returns to `idle`, still disabled | | |
| 4.7 | Enable attempt while faulted | Refused with HTTP 409 | | |
| 4.8 | Boot with the probe disconnected | Dashboard available; `sensor_open`; heating disabled | | |
| 4.9 | Reconnect after 4.8, wait for three clean samples and acknowledge | Returns to `idle`; remains disabled | | |

## Stage 5 — Fan-only relay test

Wire the 5 V fan through its channel. Heater still disconnected.

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 5.1 | 5 V supply rating vs fan label current | Supply rating exceeds fan draw | | |
| 5.2 | Press Run fan manually | Fan channel energises; heater remains disconnected | | |
| 5.3 | Press Stop manual fan | Fan channel de-energises | | |
| 5.4 | Enable with target above ambient, then press Stop manual fan | Fan stays energised because heating overrides manual off | | |
| 5.5 | Disable | Heater de-energises immediately; fan remains energised for 10 s, then de-energises | | |

## Stage 6 — Heater integration

**Only after stages 1 through 5 all record a pass.**

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 6.1 | Stages 1–5 all passed | Confirmed | | |
| 6.2 | Mains wiring inspected, earth continuity checked | Verified | | |
| 6.3 | Heater connected, target 30 °C, enable | Both channels energise together | | |
| 6.4 | Chamber temperature rises | Reading tracks upward | | |
| 6.5 | Emergency disable responds | Heater off immediately; fan remains on for 10 s, then turns off | | |

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
| 7.2 | Output switches back on at `target - 1` | Confirmed at all three setpoints | | |
| 7.3 | No relay chatter | Minimum 5 s between normal transitions | | |
| 7.4 | Overshoot recorded above | Within acceptable range for the chamber | | |
| 7.5 | Target is reached after active heating | Heater turns off immediately; fan remains on for 5 s, then turns off | | |
| 7.6 | Set target to 0 while heating | Heater turns off immediately; fan remains on for 10 s, then turns off | | |

## Stage 8 — Wi-Fi outage during regulation

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 8.1 | Disable the access point while `heating` | Regulation continues unchanged | | |
| 8.2 | Relay state during 8.1 | Unchanged by the outage | | |
| 8.3 | Serial log during 8.1 | Reconnect attempts at 1, 2, 4, 8, 30 s | | |
| 8.4 | Restore the access point | Reconnects, dashboard resumes | | |
| 8.5 | Repeat 8.1 while `holding` | Regulation continues unchanged | | |
| 8.6 | Dashboard during outage | Shows disconnected, keeps last measurement | | |
| 8.7 | Fallback access after 30 seconds | `P1S-Heat-Setup` serves the same dashboard at 192.168.4.1 | | |

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

## Stage 9 — Power interruption while enabled

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 9.1 | Cut power while `heating` | Both channels de-energise | | |
| 9.2 | Restore power | Device boots to `idle` | | |
| 9.3 | Enabled state after restart | `false` — heating does not resume | | |
| 9.4 | Relay indicators through the restart | Off throughout | | |
| 9.5 | Target after restart | Matches the persisted value | | |
| 9.6 | Repeat three times | Same result every time | | |

## Stage 10 — Maximum target

**Only after stage 7 records stable results at all three lower setpoints.**

| # | Check | Expected | Result | Date / initials |
|---|---|---|---|---|
| 10.1 | Stage 7 stable at 30, 45, and 60 °C | Confirmed | | |
| 10.2 | Target 80 °C, enable | Regulates to setpoint | | |
| 10.3 | Peak overshoot at 80 °C (°C) | Recorded, stays below 82 | | |
| 10.4 | Over-temperature fault not triggered | No latched `overtemperature` | | |
| 10.5 | Dashboard warning above 60 °C | Visible and persistent | | |

## OTA commissioning — both relay loads disconnected

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
