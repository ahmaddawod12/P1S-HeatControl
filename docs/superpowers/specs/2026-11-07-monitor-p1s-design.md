# Monitor P1S: heating that turns itself off when the print ends

Date: 2026-11-07. Agreed with the owner in conversation on 2026-11-07.

This design **replaces the Preheat & Resume workflow**. It supersedes:

- [2026-11-01 printer preheat/resume design](2026-11-01-printer-preheat-resume-design.md)
- [2026-11-07 preheat/resume availability](2026-11-07-preheat-resume-availability-design.md)
- [2026-11-07 paused printer, target, then Resume](2026-11-07-paused-target-resume-design.md)

Statements in this document marked **(builder's call)** were not decided by the
owner; a builder may change them. Statements marked **(Claude's call, not yet
confirmed)** were proposed by Claude during design and not explicitly approved by
the owner; the owner has been asked to check them.

## 1. Why

Preheat & Resume needed the ESP32 to send a Resume command to the P1S. On the
installed firmware that command needs LAN-only mode plus Developer Mode, which
disconnects the printer from Bambu's cloud (no Handy app, no cloud printing).
The owner does not want that trade-off. There is also no supported way to send
Resume through Bambu's cloud.

What the owner actually wants is simpler: heat the chamber manually as today, and
have the controller **turn the heating off by itself when the print ends**, so it
never keeps heating an empty, finished printer. Reading printer status over the
local connection already works with the printer in normal cloud mode.

## 2. Goals

- One new dashboard button, **Monitor P1S**, that adds automatic switch-off to
  normal manual heating.
- Heating follows the printer's `gcode_state` while monitoring, per the tables in
  section 4.
- The printer stays in normal cloud mode. No Developer Mode, no LAN-only mode.
- The ESP32 reads only `gcode_state` from the printer.
- Remove everything that existed only for Preheat & Resume.
- Remove the time-server (NTP) dependency from the printer connection.

## 3. Non-goals and anti-goals

Non-goals:

- No Resume, Pause, Stop or any other printer command. The only message the
  ESP32 publishes to the printer is the `pushall` status request.
- No remaining-time / progress / layer / temperature / job-name handling. The
  owner explicitly said `gcode_state` is enough.
- No printer error/HMS handling. The printer pauses or fails on its own errors,
  and those states already switch the heater off.
- No cloud connection, no Bambu account login.
- No change to the firmware-update time-server wait in
  `components/firmware_update/firmware_transport.c` (it needs internet for
  GitHub anyway). **(Claude's call, not yet confirmed)**

Anti-goals (a failure even if everything else works):

- Monitoring turns on heating the owner did not enable. Monitoring may only
  switch heating **off**, or let the owner's own heating come back after
  monitoring itself paused it.
- A leftover FINISH or FAILED from the previous print switches heating off the
  moment the button is pressed.
- Heating continues indefinitely after the printer connection is lost.
- Any existing heater safety limit, sensor fault check or fault latch is
  weakened.
- Existing saved printer settings (IP, serial, access code) stop working after
  the update.

## 4. Behavior (decided by the owner)

### Buttons

The heating controls become four buttons:

| Button | Behavior |
|---|---|
| **Enable heating** | Unchanged: heats to the target. |
| **Monitor P1S** | New. Only usable while heating is enabled. Starts monitoring. |
| **Disable** | Unchanged, and also ends monitoring. |
| **Run fan manually** | Unchanged. |

The **Preheat & Resume** and **Stop chamber control** buttons are removed.

### When Monitor P1S is pressed

| Printer state at press | What happens |
|---|---|
| FINISH, FAILED or IDLE (left over from the last print) | Heating stays on; monitoring waits for the next print to start. |
| PREPARE or RUNNING | Heating stays on; monitoring is watching this print. |
| PAUSE | Heater is held off until the print runs again. |

### Changes while monitoring

"Active" means PREPARE, RUNNING or PAUSE. Monitoring remembers that it has seen
the print active, and that memory survives a dropped and restored connection.
A print that is already PREPARE, RUNNING or PAUSE when the button is pressed
counts as seen active **(Claude's call, not yet confirmed)**.

| Change | Heater | Monitoring |
|---|---|---|
| Active → FINISH | Off | Ends; heating disabled |
| Active → FAILED (includes PAUSE → FAILED and PREPARE → FAILED) | Off | Ends; heating disabled |
| Active → IDLE (unexpected) | Off | Ends; heating disabled, same as FAILED **(Claude's call, not yet confirmed)** |
| RUNNING or PREPARE → PAUSE | Held off | Continues |
| PAUSE → RUNNING (or PREPARE) | Back on automatically | Continues |

FINISH, FAILED or IDLE only end monitoring **after** monitoring has seen the
print active. A FINISH that was already showing at press time does nothing.

Example: the printer last finished a print and still shows FINISH. The owner
enables heating, presses Monitor P1S, then sends a new print. The state goes
FINISH → PREPARE → RUNNING → FINISH. Heating stays on throughout and switches
off at the final FINISH.

### Other ways monitoring ends or holds the heater

Agreed with the owner:

- **Printer connection lost** (at any point, including while waiting for the
  print to start): heater held off. If the connection comes back and the print
  is still active (PREPARE/RUNNING), or monitoring is still waiting for the print
  to start, heating comes back automatically.
  If the print finished or failed in the meantime, the first status after
  reconnecting ends monitoring as in the table above.
- **No print starts within 60 minutes of pressing** (state never becomes active):
  monitoring ends and heating is disabled.

Claude's calls, not yet confirmed by the owner:

- **Pressing Monitor P1S again** stops monitoring and leaves manual heating
  running (toggle, like the manual-fan button).
- **Heater fault, sensor fault, maintenance mode or firmware update** that
  disables heating also ends monitoring.
- **Saving or forgetting printer settings while monitoring** ends monitoring and
  disables heating, because the controller can no longer tell when the print
  ends.
- **Reboot:** monitoring is not saved; after a reboot heating is off as today.
- **Printer state unknown or unrecognized** while monitoring (for example right
  after reconnecting, before the first status arrives): treat it the same as a
  lost connection (heater held off, monitoring continues).
- **Starting monitoring** requires that a printer is configured and enabled and
  that the controller is connected and knows the current `gcode_state`.
  Otherwise the button is unavailable and the dashboard says why.
- **Belt and braces for silent connections:** while the print is active, if no
  report at all arrives for 5 minutes, treat it as a lost connection.

## 5. Printer connection changes

### Report parsing

- Parse only `print.gcode_state` from `push_status` reports (keep the existing
  check that `print.command` is `push_status`). All other fields are skipped.
- State mapping stays as today: `IDLE`, `PREPARE`/`SLICING` → preparing,
  `RUNNING`, `PAUSE`, `FINISH`, `FAILED`; anything else is unknown.
- Remove job identity, pause/error/identity history, progress, remaining time,
  layer, temperatures, `print_error`, `hms` and `print_type` from the parser,
  the status model and the API.

### Full status request (`pushall`)

- Send one `pushall` after the first successful connection, and after each
  reconnect while `gcode_state` is unknown.
- Never more often than once every 5 minutes (P1-series guidance). No periodic
  `pushall` once the state is known.
- "Status complete" now means only "`gcode_state` known".

### Time server

- Remove the time-server wait from the printer connection
  (`clock_service_wait` in `components/printer_link/printer_transport.c`).
  Certificate expiry checking (`CONFIG_MBEDTLS_HAVE_TIME_DATE`) is off in every
  build configuration, so TLS never used the clock. Keep verifying the printer
  certificate against the bundled Bambu CA and the printer serial.
- Remove the `ntp_host` setting from the printer settings, the API and the
  dashboard ("Time server" field).
- Existing saved printer records (`printer_v1`) must still load. Keeping the
  stored layout and ignoring the old time-server bytes is acceptable
  **(builder's call)**.
- Correct docs that say verified TLS needs a valid clock.

### Connection robustness (Claude's call, not yet confirmed)

- Only the latest `gcode_state` matters. When the report queue is full, keep the
  newest state instead of tearing down and reconnecting the TLS connection.
  This also removes a suspected cause of extra printer load during file
  transfers.
- Expose a reconnect counter in `GET /api/printer` so reconnect churn can be
  observed during an upload test.

## 6. How monitoring controls the heater

- Manual heating (the existing heat controller) stays in charge of temperature.
- Monitoring adds an **inhibit**: while it is set, the heater element is off even
  though heating is still enabled. Clearing the inhibit lets the existing control
  loop continue. Ending monitoring because the print ended disables heating, as
  if the owner pressed Disable.
- The inhibit must behave like the heater switching off normally: the fan runs
  its usual cooldown after the element turns off. The manual fan button keeps
  working.
- The inhibit must fail safe. If the monitoring task stops updating while
  monitoring is on, the heater must be held off within a few seconds. Reusing the
  existing short renewable lease idea (`runtime_auto_lease`), inverted so that a
  missing renewal means "inhibit", is a reasonable approach **(builder's call)**.
- The current automatic heat lease, which lets the printer task turn heating
  **on** and blocks manual commands while active, is no longer needed. Manual
  commands are never blocked by monitoring.

## 7. Dashboard and API

Dashboard (`web/index.html`, `web/app.js`, `web/printer.js`):

- Heating controls row: Enable heating | Monitor P1S | Disable | Run fan manually.
- Monitor P1S shows a pressed state while monitoring, and is disabled when
  heating is off or the printer is unavailable, with a short reason.
- A status line shows what monitoring is doing. Examples: "Monitoring: printing",
  "Heater paused: printer paused", "Heater paused: printer connection lost",
  "Waiting for print to start (stops in 42 min)", and after it ends "Print
  finished: heating turned off". Exact wording is the **builder's call**.
- Printer card: keep printer settings and connection status, and show the
  printer state. Remove Preheat & Resume, Stop chamber control, the workflow
  hint and the Time server field.

API:

- Remove `POST /api/printer/cycle` and `POST /api/printer/cycle/stop`.
- Add endpoints to start and stop monitoring. Names and shape are the
  **builder's call** (for example `POST /api/printer/monitor` and
  `POST /api/printer/monitor/stop`).
- `GET /api/printer` returns connection status, printer state, monitoring status
  (active, what it is doing, why it last ended, waiting-time left) and the
  reconnect counter. Remove the arm token and the removed fields.
- `/api/control/disable` also ends monitoring.

## 8. What gets removed

- Resume dispatch: `printer_link_resume_once`, the dispatch guard, the
  resume-specific send fencing in `printer_transport.c` and anything in
  `printer_deadline_transport.*` that only served Resume.
- The cycle state machine and arm tokens: `printer_cycle.c/.h`, the cycle parts
  of `printer_control.c`, and the automatic heat lease in the runtime once the
  inhibit replaces it.
- Job identity, history and the extra report fields (section 5).
- The time-server wait and `ntp_host` setting (section 5).
- Tests that only covered removed behavior, including the
  `preheat-resume` test configuration, and replace them with tests for the new
  behavior.

## 9. Docs

- Mark the three superseded design docs (top of this file) and the 2026-11-01
  implementation plan as superseded by this design. Keep their history.
- `docs/printer-startup-profile.md`: obsolete (no startup pause is needed). Mark
  obsolete or remove **(builder's call)**.
- `docs/printer-integration-qualification.md`: Resume qualification no longer
  applies; keep recorded evidence as history.
- `README.md`, `docs/commissioning-checklist.md`,
  `docs/printer-connection-test.md`: describe Monitor P1S, drop Developer Mode,
  startup-pause and time-server instructions.

## 10. Verification

- Host tests for the monitoring state machine covering every row of both tables
  in section 4, the leftover-FINISH case, connection loss and recovery
  (including a print that finishes while disconnected), the 60-minute wait,
  unknown state, the 5-minute silence rule, Disable, faults and settings changes.
- Inhibit fail-safe test: monitoring task stops updating → heater held off.
- Parser tests: only `gcode_state` is read; other fields are ignored.
- `pushall` tests: first connect, reconnect with unknown state, 5-minute limit.
- Connection test without any time server.
- Device config test: an old saved `printer_v1` record with a time server still
  loads with IP, serial and access code intact.
- Dashboard tests (`test_apps/web`): button availability, toggle, status lines,
  removed controls and fields.
- Run the full host, browser and tool suites, and build the release firmware and
  the Unity test app.
- Do not flash the board, change printer settings or claim behavior on hardware
  without separate permission and a verified install.

## 11. Verification results — 2026-11-08

Implemented on `main` per [the plan](../plans/2026-11-08-monitor-p1s.md).
Actual results; nothing was flashed and no hardware behavior is claimed.

| Check | Result |
|---|---|
| Host suite (MinGW + CTest) | 102/102 passed. Baseline before the change was 147/147; the Resume/cycle/history/native-guard/lease suites were removed and monitor, permit, control, link and report cases were added. |
| Dashboard (`node --test test_apps/web/*.cjs`) | 41/41 passed |
| Packaging tool tests (ESP-IDF venv Python) | 19/19 passed |
| Release firmware (`build-release`) | Built; image 1,229,184 bytes; smallest app partition 39% free |
| Unity test app | Built; not executed on a board |
| HTTP stack check (`tools/check_printer_stack.py`) | Deepest Monitor P1S path (start → heat status) needs 1,840 of 4,096 bytes |
| Repository checks | `printer_link` publishes only `pushall`; no `clock_service` in `printer_link`; no cycle, arm-token or automatic-lease code remains |
| Hardware | Not flashed; live Monitor P1S trials remain in the commissioning checklist |
