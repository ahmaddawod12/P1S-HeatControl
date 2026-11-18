# P1S Chamber Heat Controller

ESP-IDF firmware for an ESP32 Dev Kit (ESP32-WROOM-32) that reads a three-wire
DS18B20 digital temperature probe over 1-Wire, switches a 220 VAC / 350 W
heater and a 5 V fan through a two-channel relay board, and serves a standalone
local dashboard over Wi-Fi.

The firmware targets the classic `esp32` chip with ESP-IDF v6.1,
4 MB flash, and the UART0 console through the board's USB-to-UART
bridge. The DS18B20 DATA input uses bidirectional GPIO27; GPIO16 and GPIO17
control the heater and fan relays.

The local dashboard includes signed firmware updates from this public GitHub
repository and optional verified local MQTT monitoring for a
Bambu P1S. There is no application password. Everything the browser loads is
served from the device. **Monitor P1S** turns manual heating off by itself
when the print ends; it only reads the printer state and never sends print
commands. Live monitoring trials are still pending.

> **Mains warning.** The heater channel switches 220 VAC. Commission the board
> with both relay contacts unloaded and work through
> [docs/commissioning-checklist.md](docs/commissioning-checklist.md) in order.
> Do not connect the heater until every earlier stage records a pass.

## Hardware

| Signal | GPIO | Notes |
|---|---|---|
| Heater relay | GPIO16 | Runs continuously while enabled and below the target |
| Fan relay | GPIO17 | Runs with the heater, manually, for 5 s after reaching the target, and for 10 s after disable |
| DS18B20 DATA | GPIO27 | Digital 1-Wire; pull up to 3.3 V through 4.7 kΩ |

When replacing the NTC, disconnect its divider and filter capacitor. Connect
DS18B20 DATA to GPIO27, VCC to 3.3 V and GND to ESP32 GND. GPIO34 is input-only
and cannot drive 1-Wire. Connect the heater relay input to GPIO16 and the fan
relay input to GPIO17. Use the GPIO labels on the board, not the header
positions. GPIO6–11 are used by flash.
See [Espressif's DevKitC pin table](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html).

The heater and circulation fan are controlled separately. Enabling below the
target starts the fan before the heater within the same control iteration. The
heater then runs continuously until the target is reached. At the target, the
heater turns off immediately and the fan continues for 5 seconds before
turning off. Disabling heating turns the heater off immediately and keeps the
fan on for 10 seconds. A latched fault stops heating and keeps the fan running
for 10 seconds when it ends an enabled heating session.
A dashboard manual-fan request remains an independent override. Neither
channel is driven active during reset: the pins
are pulled to their inactive level and the output register is preloaded before
the driver is enabled.

### Sensor

| Setting | Value |
|---|---|
| Probe | One externally powered DS18B20 waterproof probe and terminal adapter |
| DATA / yellow probe lead | GPIO27 |
| VCC / red probe lead | 3.3 V |
| BLK / black probe lead | GND, shared with the ESP32 |
| DATA pull-up | 4.7 kΩ from DATA to 3.3 V; use the adapter resistor if fitted |
| Resolution | 11 bits, 0.125 °C increments |
| Conversion | Fresh conversion before every CRC-checked reading; driver waits 400 ms |
| Sensor IC range | −55 to 125 °C |
| Supplied cable limit | 85 °C maximum |
| Sample period | 1 s, including conversion time |
| Controller checks | Every 0.5 s; queued commands can wake it sooner |
| Sensor fault threshold | 5 consecutive failed reads; any successful read resets the count |
| Acquisition task stale after | 2 s without a completed read, including failed reads |
| Samples required before enabling | 3 consecutive valid |

Follow the adapter's printed DATA/VCC/BLK labels; jumper-cable colors and header
positions can differ. The listing describes a pull-up adapter but does not
specify its resistor value: verify the fitted resistor before wiring. If it
has no pull-up, add 4.7 kΩ between DATA and 3.3 V. Power the adapter from 3.3 V
so its DATA pull-up also uses 3.3 V. Do not reuse the NTC's 10 kΩ divider or
100 nF filter capacitor on the DATA line.

The 85 °C cable limit is lower than the chip's rated maximum. The controller's
80 °C maximum target and 82 °C shutdown remain; route the cable away from the
heater and hotter surfaces. Verify overshoot during commissioning.

Readings pass through a five-value median filter plus a persisted calibration
offset limited to ±10 °C, defaulting to 0 °C. Use **Sensor calibration → Save
offset** on the dashboard to adjust it without restarting. Positive offsets
raise the measured temperature; negative offsets lower it. The correction
applies to both displayed readings and heater control, and survives restarts
and firmware updates. On the first upgrade from NTC firmware, settings
schema 1 migrates to schema 2: the target is preserved and the old NTC offset
resets to 0 °C. A new DS18B20 calibration then persists normally. Wi-Fi settings
are stored separately and are unaffected.

The independent controller continues running while the sensor task waits for
conversion. Missing presence, stuck-low DATA, CRC/transport errors and invalid
readings count toward the same five-failure streak. During failures 1–4 the
heater retains its existing output; invalid temperatures never enter the filter.
One successful read resets the failure count. Failure 5 latches the sensor fault,
disables heating and starts a 10-second fan cooldown for an enabled session.
If the acquisition task stops completing reads, the independent two-second
stale-task timeout still shuts heating down, including a stall after a bad read.
A gap over two seconds clears the old filter and valid-sample streak. A new raw
or filtered reading at or above 82 °C shuts heating down immediately, without
waiting for five failures. This requires a usable temperature reading; retries
intentionally retain heating briefly without fresh temperature feedback.
Reconnect the probe, wait for three clean samples, then acknowledge a latched
fault; heating remains disabled until explicitly enabled. An enabled heater
that is already off requires three clean samples and the five-second restart
interval before regulation can turn it on again.
See the [DS18B20 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/DS18B20.pdf)
and [migration design](docs/superpowers/specs/2026-11-01-ds18b20-sensor-design.md).

### Relay polarity

The current configuration is **active high**: `false` (OFF) drives LOW (`0`),
and `true` (ON) drives HIGH (`1`). Heater GPIO16 and fan GPIO17 initialize LOW,
with heating disabled after every boot.

For active-low relay inputs, enable the polarity option with:

```powershell
idf.py menuconfig
```

under **P1S Heat Controller Board → Relay channels are active low**
(`CONFIG_P1S_RELAY_ACTIVE_LOW`).

## Control behaviour

| Setting | Value |
|---|---|
| Target range | 0 to 80 °C in 0.5 °C steps |
| Default target | 60 °C |
| Target 0 | Disables heating |
| Hysteresis | 1 °C — on at or below `target - 1`, off at the target |
| Heater operation | Continuous while enabled and below the target |
| Minimum restart interval | 5 s; target and safety shutoffs remain immediate |
| Circulation fan | On with the heater or manually, plus 5 s after reaching the target or 10 s after disable |
| Over-temperature | Latched fault at 82 °C from raw **or** filtered temperature |

Wi-Fi credentials, target, calibration offset, heating enable state, and optional
printer connection settings are saved in flash. Both relays start inactive at
boot. Previously enabled heating resumes after three consecutive valid sensor
samples if startup checks pass. If heating was disabled or has never been saved,
it stays disabled. Active faults and relay states are transient.

Acknowledging a fault returns to IDLE and never resumes heating. Sensor faults
need three consecutive clean samples; an over-temperature fault needs the
chamber below 80 °C; an output-initialisation fault cannot be cleared in
software.

Regulation keeps running through a Wi-Fi outage.

### Optional Monitor P1S

Open **Printer connection** on the dashboard and save the printer IPv4, serial
and LAN access code. The access code is write only; omit it to preserve the
saved code only for the same serial. The ESP32 connects to the printer's local
MQTT over TLS and verifies the printer certificate against the Bambu CA and the
serial. The printer stays in its normal cloud mode: no LAN-only mode, Developer
Mode or time server is needed, and Bambu Handy and cloud printing keep working.
An unavailable printer does not block manual chamber control or dashboard
startup.

For a first connection, follow the [Wi-Fi change and printer monitoring test](docs/printer-connection-test.md)
with relay loads disconnected before attempting any heated integration.

The heating controls are **Enable heating**, **Monitor P1S**, **Disable** and
**Run fan manually**. Monitor P1S is available while heating is enabled and the
printer state is known, and turns the heater off by itself when the print ends:

| Printer state when Monitor P1S is pressed | What happens |
|---|---|
| FINISH, FAILED or IDLE (left over from the last print) | Heating continues and waits for the next print to start |
| PREPARE or RUNNING | Heating continues and watches this print |
| PAUSE | Heater held off until the print runs again |

| While monitoring | Heater | Monitoring |
|---|---|---|
| The watched print ends in FINISH or FAILED, or drops to IDLE | Off | Ends; heating disabled |
| RUNNING or PREPARE → PAUSE | Held off | Continues |
| PAUSE → RUNNING | Back on | Continues |
| Connection lost, or no report for 5 minutes while printing | Held off | Continues; heating returns with the connection |
| No print starts within 60 minutes of pressing | Off | Ends; heating disabled |

Firmware update requests are rejected while monitoring is active. Stop
**Monitor P1S** before updating; stopping it keeps manual heating enabled.
Pressing **Monitor P1S** again stops monitoring and keeps heating. **Disable**,
a fault or maintenance turn heating off and end monitoring;
saving or forgetting printer settings while monitoring ends it and turns
heating off. Monitoring is never restored after a reboot. While monitoring,
the heater may only run while the monitor keeps renewing a 5-second permit, so
a stalled monitor holds the heater off.

The ESP32 reads only `gcode_state`. The only message it sends to the printer is
the `pushall` status request: once when it connects and, while the state is
unknown, at most once every 5 minutes. Report bursts or unreadable reports
never force a reconnect; `GET /api/printer` exposes a `reconnects` count for
diagnosis.

## Network

The controller joins the network saved in the dedicated
`device_cfg` NVS partition, reconnects with 1, 2, 4, 8, then 30-second
backoff for as long as the router stays away, and advertises `_http._tcp` on
port 80 at its saved `p1s-<name>.local` address.

Use **Wi-Fi settings** in the dashboard to view and edit the saved network name
and password as plain text. **Save & Connect** saves the exact text, including
spaces, and connects immediately without restarting. Leave the password empty
for an open network. The optional **Controller name** field previews the
address: `left` becomes `http://p1s-left.local/`. Names accept up to 22 letters,
numbers, or internal hyphens; surrounding whitespace is trimmed and letters
are saved in lowercase. An empty name starts at `p1s-1.local`.
Wi-Fi changes and firmware updates run one at a time.

On the selected network, mDNS checks the name for conflicts. If a custom name
is taken, the controller tries `p1s-1.local`, `p1s-2.local`, and so on. After
confirmation it saves the final name and displays the actual link. Reboots
and OTA updates retain that name; later conflicts trigger another check and
save. Existing Wi-Fi credentials are retained when upgrading.

With no saved credentials, the open **P1S-Heat-Setup** hotspot starts immediately.
If a saved network cannot be reached, it starts after 30 seconds while station
retries continue. Join that hotspot and open **http://192.168.4.1/** for the same
dashboard. Incorrect credentials leave setup access available for correction.
After connection succeeds, rejoin your normal Wi-Fi and use the displayed
controller address; the hotspot closes 10 seconds after the address is confirmed.
If the address changed before your browser learned it, join your new Wi-Fi and
open the discovery link shown in Wi-Fi settings. During a name-only change,
the IP link stays available. If the name was taken before the browser learned
the numbered address, reopen setup to read the final name.

A personal USB bootstrap can still seed credentials once from
`sdkconfig.secret`. Existing saved credentials take precedence; normal OTA
releases contain no compiled Wi-Fi credentials. Saved credentials survive
restarts and OTA updates. A first setup with no rollback image can complete
after the two-minute health deadline through this page without a restart.

Regulation is independent of all of it. A board that never joins, or one whose
router disappears mid-run, keeps regulating on its own. Network changes never
enable heating or clear a fault.

## REST API

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/status` | Full status snapshot |
| GET | `/api/diagnostics` | Last 32 transitions and faults |
| PUT | `/api/target` | `{"target_c": 60.0}` |
| PUT | `/api/calibration` | Save and apply `{"calibration_offset_c": -1.5}`; range −10 to +10 °C, default 0 |
| POST | `/api/control/enable` | Enable heating |
| POST | `/api/control/disable` | Disable heating |
| POST | `/api/control/fan/enable` | Run the fan manually |
| POST | `/api/control/fan/disable` | Clear the manual fan request |
| POST | `/api/fault/acknowledge` | Clear a latched fault |
| GET | `/api/events` | Server-Sent Events status stream |
| GET | `/api/firmware` | Local version, update progress, outcome and per-boot CSRF token |
| POST | `/api/firmware/update` | Start one update, with `{}`, matching Origin and `X-P1S-CSRF` |
| GET | `/api/wifi` | Visible saved credentials, connection/hotspot status and CSRF token; no-store |
| PUT | `/api/wifi` | Save `{"ssid":"network","password":"password"}` with matching Origin and `X-P1S-CSRF`; returns 202 |
| GET | `/api/printer` | Redacted connection settings, printer state, reconnect count, monitoring status and CSRF token; no-store |
| PUT | `/api/printer/config` | Admit printer settings save; 202; preserve an omitted access code only for the same serial |
| DELETE | `/api/printer/config` | Admit forgetting only the printer record; 202; `applying`/`config_result` report storage completion |
| POST | `/api/printer/monitor` | Start Monitor P1S with `{}`; 204, or 409 with the reason when it is not available |
| POST | `/api/printer/monitor/stop` | Stop monitoring and keep heating; 204 |
| GET | `/`, `/styles.css`, `/app.js`, `/wifi.js`, `/printer.js` | Dashboard |

All chamber mutations (`/api/target`, `/api/calibration`, `/api/control/*`, and `/api/fault/acknowledge`)
require a matching local `Origin` and `X-P1S-CSRF`, using the per-boot token from
`GET /api/firmware`. This applies through both the normal network and setup
hotspot. Missing or mismatched headers return `403`. The dashboard reads the
current token before each deliberate chamber command; it never retries a mutation.
Target bodies must contain exactly one numeric `target_c` field. Nested input
is rejected before recursive JSON parsing; duplicate, unknown and trailing
input is also rejected.
Calibration bodies follow the same rules with one finite numeric
`calibration_offset_c` field from −10 to +10 °C. Status and SSE include the
active `calibration_offset_c` value.
Bodies remain limited to 256 bytes. A receive timeout or expired five-second
body deadline returns `408` and releases the HTTP task for other requests.
The same five-second deadline applies to firmware, Wi-Fi and printer bodies,
including each blocking socket read. Rejected or incomplete bodies close the
connection instead of being drained. GET routes reject nonempty bodies.
The chamber control and fault-acknowledgement POSTs require empty bodies;
nonempty bodies return `400` and close the connection before any command runs.

Successful commands return `204`. Validation failures return `400`, enable and
acknowledge precondition failures `409`, an unavailable command queue `503`,
and internal errors `500`. At most four SSE clients are served at once; a
status frame goes out whenever the status changes, and `: keepalive` after
fifteen quiet seconds.

Fault acknowledgement waits for the controller's decision and published status.
An uncleared sensor fault, a chamber still too hot, or an output-initialisation
fault returns `409`; a controller response timeout returns `503`. Requests that
expire before execution do not clear a fault later. Successful acknowledgement
leaves heating disabled. `/api/diagnostics` retains the last 32 controller state
or fault changes for the current runtime, without repeating unchanged faults.

Firmware update requests return `202` when accepted, `409` while busy or in
trial maintenance, and `503` when unavailable. Maintenance also rejects heat
enable, target/calibration changes, manual fan enable and fault acknowledgement with `409`.
The LAN dashboard has no account authentication; the update route checks the
request's local origin and per-boot token. All printer mutations use the same
Origin/`X-P1S-CSRF` protection, with 400 for invalid input, 403 for an untrusted
request, 409 for a conflicting/ineligible request, and 503 for unavailability.
Printer inputs are limited to 1,024 bytes and status to 2,048 bytes. The existing
512-byte status/SSE contract is unchanged. The printer settings fields are
`enabled`, `ipv4`, `serial` and optional `access_code`. Unknown/duplicate
fields (including the retired `ntp_host`) and trailing JSON are rejected.

State names on the wire: `boot`, `idle`, `heating`, `holding`, `fault`. Fault names: `none`, `sensor_open`, `sensor_short`, `sensor_range`,
`sensor_invalid`, `sensor_stale`, `overtemperature`, `nvs`, `output_init`.

## Firmware updates

After the initial USB OTA bootstrap, press **Update firmware** in the dashboard.
The board checks the newest stable public release, downloads it over verified
HTTPS, checks its board/layout/version, digest and ECDSA signature, and writes
the inactive OTA slot. It waits for the normal fan cooldown before writing.
Heating stays disabled through the update and after reboot. A trial image must
pass local runtime, storage, Wi-Fi and HTTP health checks within two minutes;
otherwise it rolls back when a previous confirmed image is available.

See [the OTA release guide](docs/ota-release-guide.md) for signing, public
packaging, USB setup and recovery. Source and releases share
[`ahmaddawod12/P1S-HeatControl`](https://github.com/ahmaddawod12/P1S-HeatControl).
Version **1.0.0** starts the public source release series. Devices running the
previous development versions, including 1.0.5, need a signed USB installation
to adopt the reset version, new update repository and active-high relay profile.
Subsequent newer releases use dashboard OTA normally.

To create a release, bump `version.txt`, add `docs/releases/v<version>.md`, and
merge into `main`. The **Release firmware** GitHub Actions workflow builds,
tests, signs and publishes that version. You can also start it from
**Actions → Release firmware → Run workflow** on `main`. Set up the existing
signing key as an Actions secret once, as described in the
[release guide](docs/ota-release-guide.md#release-through-github-actions).
Ordinary pushes without a version change do not publish a release.

See the [historical implementation status](docs/ota-implementation-status.md)
for earlier board results and remaining physical commissioning checks.

## Build and flash

### Wi-Fi credentials

Only the personal bootstrap compiles credentials from `sdkconfig.secret`, which
is gitignored and must never be committed. Create it beside `sdkconfig.defaults`:

```ini
CONFIG_P1S_WIFI_SSID="Your network"
CONFIG_P1S_WIFI_PASSWORD="your-passphrase"
```

Set `P1S_BOOTSTRAP_CREDENTIALS=ON` explicitly in a separate bootstrap build.
Generic builds default to `OFF` and read saved credentials on the device.
The passphrase must be 8 to 63 bytes, or empty for an open network.

A compiled-in passphrase is recoverable from the flash image by anyone who can
read the chip. Treat a flashed board as carrying the network password.

Run these from an ESP-IDF v6.1 PowerShell environment:

```powershell
idf.py -B build-bootstrap -D SDKCONFIG=sdkconfig.bootstrap -D P1S_BOOTSTRAP_CREDENTIALS=ON build
# Prepare, sign and verify the personal image; this does not write a board.
./tools/install_bootstrap.ps1 -BuildDir build-bootstrap -PrivateKey 'C:/path/outside-checkout/ota_signing_private.pem'
# See the release guide before adding an explicit -Port COMx -Flash.
idf.py -C test_apps/unit set-target esp32
idf.py -C test_apps/unit build
$p1sPort = 'COMx' # Replace with the explicitly selected board's port.
pytest test_apps/unit/pytest_unit.py --target esp32 --embedded-services esp,idf --port $p1sPort -s
```

`espressif/mdns`, `espressif/cjson`, `espressif/ds18b20` (0.4.0) and
`espressif/onewire_bus` (1.1.2) are pulled in by the component manager
on the first build; everything else is built into ESP-IDF.

### Before the first test run

`pytest` is **not** part of the ESP-IDF v6.1 Python environment. Install the
runner once, from an activated ESP-IDF environment:

```powershell
python -m pip install pytest-embedded-serial-esp pytest-embedded-idf
```

Close any `idf.py monitor` first — it holds the serial port exclusively and
pytest fails with `Could not open COM… Access is denied`.

The pytest runner flashes the Unity test image onto the board. Restore the
signed personal bootstrap with `install_bootstrap.ps1` after the suite completes.

### Clean rebuild and full suite

```powershell
idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D P1S_BOOTSTRAP_CREDENTIALS=OFF build
idf.py -C test_apps/unit fullclean
idf.py -C test_apps/unit set-target esp32
idf.py -C test_apps/unit build
$p1sPort = (Get-CimInstance Win32_SerialPort | Where-Object Description -Match 'USB|JTAG|Serial' | Select-Object -First 1 -ExpandProperty DeviceID)
pytest test_apps/unit/pytest_unit.py --target esp32 --embedded-services esp,idf --port $p1sPort -s
```

### Sensor smoke test

With both relay contacts unloaded and the DS18B20 adapter connected to GPIO27,
3.3 V and GND as described above:

```powershell
idf.py -C test_apps/sensor_smoke set-target esp32
idf.py -C test_apps/sensor_smoke build
idf.py -C test_apps/sensor_smoke -p $p1sPort flash monitor
```

This image keeps both relays off and checks ten ambient-temperature samples.
Restore the signed personal bootstrap after testing.

### Host tests without a board

The DS18B20 adapter with hardware stubs, settings migration, filtering, and
the state machine also build and run on the PC. This is a fast pre-check, **not** a
substitute for the on-target Unity run.

Run an ESP-IDF build once to download the managed cJSON dependency before
configuring the host tests.

```powershell
$p1sCmake = 'cmake'
& $p1sCmake -S test_apps/host -B test_apps/host/build -G 'MinGW Makefiles'
& $p1sCmake --build test_apps/host/build
& $p1sCmake --build test_apps/host/build --target test
node --test test_apps/web/*.cjs
python -m unittest discover -s test_apps/tools -v
./test_apps/tools/test_publish_firmware.ps1
```

After building `build-release`, run `python tools/check_printer_stack.py --build build-release`
to check the compiled HTTP Monitor P1S paths against their stack budget.
Actual source verification results and pending physical checks are recorded in
[printer qualification](docs/printer-integration-qualification.md).

## Startup order

`app_main` runs a fixed sequence and commands the relays off on any failure:

1. `board_outputs_init` — relays before anything else
2. OTA trial supervisor — boot maintenance and startup watchdog when unconfirmed
3. `nvs_flash_init` and `settings_init`
4. `app_runtime_start`
5. `wifi_manager_start` — dedicated device configuration and saved Wi-Fi
6. `web_server_start`, then the update worker
7. Optional printer coordinator — saved connection settings restore the printer
   connection only; Monitor P1S starts off. Heating restores its saved enable
   state once the sensor has reported three valid samples and startup checks pass.

A relay or runtime failure stops startup with the outputs off. A networking or
web-server failure leaves the runtime regulating and retries in the background.

## Layout

| Path | Responsibility |
|---|---|
| `main/` | Startup composition |
| `components/app_types/` | Shared enums and status structures |
| `components/temperature_sensor/` | DS18B20 acquisition over RMT 1-Wire, probe faults |
| `components/temperature/` | Median filtering, calibration, validity, staleness |
| `components/controller/` | Pure state machine |
| `components/settings/` | Versioned NVS schema |
| `components/device_config/` | Dedicated Wi-Fi, update-attempt and optional printer NVS records |
| `components/firmware_update/` | Signed OTA transport, worker and boot confirmation |
| `components/board/` | Independent relay output safety and ordering |
| `components/runtime/` | Tasks, queues, the fail-safe printer-monitoring heat permit, and the only relay handle |
| `components/clock_service/` | Cancellable time synchronization before the GitHub firmware-update check |
| `components/printer_link/` | `gcode_state`-only reports, rate-limited `pushall` and verified local MQTT transport |
| `components/printer_control/` | Monitor P1S state machine and coordinating worker |
| `components/diagnostics/` | 32-entry transition ring buffer |
| `components/wifi_manager/` | Saved credentials, fallback hotspot, async changes, reconnect, mDNS |
| `components/web_server/` | JSON contract, REST routes, SSE hub |
| `components/app_startup/` | Injectable startup order and failure policy |
| `web/` | Embedded dashboard assets |
| `test_apps/unit/` | On-target Unity suite |
| `test_apps/host/` | MinGW/CTest runner for pure logic |
| `tools/` | Verified release packaging, publication and personal USB setup |
