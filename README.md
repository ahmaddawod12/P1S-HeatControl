# P1S Chamber Heat Controller

ESP-IDF firmware for an ESP32 Dev Kit (ESP32-WROOM-32) that reads a three-wire
DS18B20 digital temperature probe over 1-Wire, switches a 220 VAC / 350 W
heater and a 5 V fan through a two-channel relay board, and serves a standalone
local dashboard over Wi-Fi.

The firmware targets the classic `esp32` chip with ESP-IDF v6.1,
4 MB flash, and the UART0 console through the board's USB-to-UART
bridge. The DS18B20 DATA input uses bidirectional GPIO27; GPIO25 and GPIO26
control the heater and fan relays.

The local dashboard includes signed firmware updates from a separate public
GitHub releases repository and optional verified local MQTT monitoring for a
Bambu P1S. There is no application password. Everything the browser loads is
served from the device. Automatic preheat/Resume is implemented behind a
default-off qualification gate; the owner's printer workflow is still untested.

> **Mains warning.** The heater channel switches 220 VAC. Commission the board
> with both relay contacts unloaded and work through
> [docs/commissioning-checklist.md](docs/commissioning-checklist.md) in order.
> Do not connect the heater until every earlier stage records a pass.

## Hardware

| Signal | GPIO | Notes |
|---|---|---|
| Heater relay | GPIO25 | Runs continuously while enabled and below the target |
| Fan relay | GPIO26 | Runs with the heater, manually, for 5 s after reaching the target, and for 10 s after disable |
| DS18B20 DATA | GPIO27 | Digital 1-Wire; pull up to 3.3 V through 4.7 kΩ |

When replacing the NTC, disconnect its divider and filter capacitor. Connect
DS18B20 DATA to GPIO27, VCC to 3.3 V and GND to ESP32 GND. GPIO34 is input-only
and cannot drive 1-Wire. The relay inputs remain on GPIO25 and GPIO26. Use the
GPIO labels on the board, not the header positions. GPIO6–11 are used by flash.
See [Espressif's DevKitC pin table](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html).

The heater and circulation fan are controlled separately. Enabling below the
target starts the fan before the heater within the same control iteration. The
heater then runs continuously until the target is reached. At the target, the
heater turns off immediately and the fan continues for 5 seconds before
turning off. Disabling heating turns the heater off immediately and keeps the
fan on for 10 seconds. A fault turns both automatic outputs off immediately.
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
| Sample period | 500 ms, including conversion time |
| Stale after | 2 s |
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
offset limited to ±10 °C. On the first upgrade from NTC firmware, settings
schema 1 migrates to schema 2: the target is preserved and the old NTC offset
resets to 0 °C. A new DS18B20 calibration then persists normally. Wi-Fi settings
are stored separately and are unaffected.

The independent controller continues running while the sensor task waits for
conversion and enforces the two-second freshness timeout even if acquisition
stalls. A gap over two seconds clears the old filter and valid-sample streak.
A missing probe, stuck-low DATA, CRC/transport error or invalid
reading inhibits heating. Reconnect the probe, wait for three clean samples,
then acknowledge the fault; heating remains disabled until explicitly enabled.
See the [DS18B20 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/DS18B20.pdf)
and [migration design](docs/superpowers/specs/2026-11-01-ds18b20-sensor-design.md).

### Relay polarity

Most opto-isolated relay boards energise on a low input, which is the default.
Change it with:

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

Persisted: Wi-Fi credentials, target, calibration offset, and optional printer
connection settings. **Never**
persisted: enabled state, active fault, runtime state, or relay state. Both
relays start inactive on every boot and reset, and heating never resumes
automatically — someone has to enable it again.

Acknowledging a fault returns to IDLE and never resumes heating. Sensor faults
need three consecutive clean samples; an over-temperature fault needs the
chamber below 80 °C; an output-initialisation fault cannot be cleared in
software.

Regulation keeps running through a Wi-Fi outage.

### Optional P1S monitoring and preheat/Resume

Open **Printer connection** on the dashboard and save the printer IPv4, serial,
LAN access code and reachable time server. The access code is write only; omit
it to preserve the saved code only for the same serial. Monitoring uses verified
TLS on the local network. An unavailable printer does not block manual chamber
control or dashboard startup.

The intended automated workflow is: the PC prepares and submits the complete
job with a qualified initial pause before all extrusion; the operator confirms
that pause and selects **Preheat & Resume**; the ESP32 reaches the existing
target, settles within ±1 °C for 30 seconds, and permits one Resume attempt.
It maintains chamber control until actual printer finish. It never selects a
file or carries print options. A file merely open on the PC is not a resumable
job.

**Automatic Resume is currently unavailable in generic builds.** P1S firmware,
Bambu Studio version, exact pause command/placement and live timing/resources
still require qualification. `CONFIG_P1S_PRINTER_AUTO_RESUME` defaults to `n`.
See [qualification](docs/printer-integration-qualification.md) and the
[PC startup profile procedure](docs/printer-startup-profile.md).

Active automation captures the target and uses a revocable heat lease, renewed
every second for at most five seconds. The independent 100 ms runtime enforces
expiry. Connection/status loss, a thermal fault, later printer pause, changed
job, cancellation or uncertain Resume ends chamber control. Reconnect and
reboot do not restore a cycle. **Stop chamber control** and **Disable** revoke
automation without cancelling the printer job; manual-fan control remains
independent. Wi-Fi changes and OTA cannot overlap an active cycle.

## Network

The controller joins the network saved in the dedicated
`device_cfg` NVS partition, reconnects with 1, 2, 4, 8, then 30-second
backoff for as long as the router stays away, and advertises `_http._tcp` on
port 80 as `p1s-heat.local`.

Use **Wi-Fi settings** in the dashboard to view and edit the saved network name
and password as plain text. **Save & Connect** saves the exact text, including
spaces, and connects immediately without restarting. Leave the password empty
for an open network. Wi-Fi changes and firmware updates run one at a time.

With no saved credentials, the open **P1S-Heat-Setup** hotspot starts immediately.
If a saved network cannot be reached, it starts after 30 seconds while station
retries continue. Join that hotspot and open **http://192.168.4.1/** for the same
dashboard. Incorrect credentials leave setup access available for correction.
After connection succeeds, rejoin your normal Wi-Fi and use the displayed
controller address; the hotspot closes 10 seconds after connection.
If the address changed before your browser learned it, join your new Wi-Fi and
open `http://p1s-heat.local/`. That discovery link stays visible in the page.

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
| GET | `/api/printer` | Redacted connection/readiness, printer/job/progress, cycle/outcome, offered arm token and CSRF token; no-store |
| PUT | `/api/printer/config` | Admit printer settings save; 202; preserve an omitted access code only for the same serial |
| DELETE | `/api/printer/config` | Forget only the printer record; 204 after storage confirms the clear |
| POST | `/api/printer/cycle` | Admit `{"arm_token":"…","initial_pause_confirmed":true}` using the existing chamber target; 202 |
| POST | `/api/printer/cycle/stop` | Revoke chamber automation; 204; does not cancel the printer job |
| GET | `/`, `/styles.css`, `/app.js`, `/wifi.js`, `/printer.js` | Dashboard |

All chamber mutations (`/api/target`, `/api/control/*`, and `/api/fault/acknowledge`)
require a matching local `Origin` and `X-P1S-CSRF`, using the per-boot token from
`GET /api/firmware`. This applies through both the normal network and setup
hotspot. Missing or mismatched headers return `403`. The dashboard reads the
current token before each deliberate chamber command; it never retries a mutation.
Target bodies must contain exactly one numeric `target_c` field. Nested input
is rejected before recursive JSON parsing; duplicate, unknown and trailing
input is also rejected.
Bodies remain limited to 256 bytes. A receive timeout or expired five-second
body deadline returns `408` and releases the HTTP task for other requests.
The chamber control and fault-acknowledgement POSTs require empty bodies;
nonempty bodies return `400` and close the connection before any command runs.

Successful commands return `204`. Validation failures return `400`, enable and
acknowledge precondition failures `409`, an unavailable command queue `503`,
and internal errors `500`. At most four SSE clients are served at once; a
status frame goes out whenever the status changes, and `: keepalive` after
fifteen quiet seconds.

Firmware update requests return `202` when accepted, `409` while busy or in
trial maintenance, and `503` when unavailable. Maintenance also rejects heat
enable, target changes, manual fan enable and fault acknowledgement with `409`.
The LAN dashboard has no account authentication; the update route checks the
request's local origin and per-boot token. All printer mutations use the same
Origin/`X-P1S-CSRF` protection, with 400 for invalid input, 403 for an untrusted
request, 409 for a conflicting/ineligible request, and 503 for unavailability.
Printer inputs are limited to 1,024 bytes and status to 2,048 bytes. The existing
512-byte status/SSE contract is unchanged. The printer settings fields are
`enabled`, `ipv4`, `serial`, optional `access_code`, and optional `ntp_host`
(default `pool.ntp.org`). Unknown/duplicate fields and trailing JSON are rejected.

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
packaging, USB setup and recovery. The source repository stays private. The
public repository `ahmaddawod12/P1S-HeatControl-Releases` contains the approved
test releases. See the [implementation status](docs/ota-implementation-status.md)
for measured board results and remaining physical commissioning checks.

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
to check the compiled HTTP arm path against its stack budget.
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
7. Optional printer coordinator — saved connection settings restore monitoring
   only; an active cycle, heater ownership and Resume intent never restore

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
| `components/runtime/` | Tasks, queues, independent automatic heat leases, and the only relay handle |
| `components/clock_service/` | Shared cancellable time synchronization for verified TLS |
| `components/printer_link/` | Bounded reports, run identity and verified local MQTT transport |
| `components/printer_control/` | Explicit per-run preheat/Resume policy and coordinating worker |
| `components/diagnostics/` | 32-entry transition ring buffer |
| `components/wifi_manager/` | Saved credentials, fallback hotspot, async changes, reconnect, mDNS |
| `components/web_server/` | JSON contract, REST routes, SSE hub |
| `components/app_startup/` | Injectable startup order and failure policy |
| `web/` | Embedded dashboard assets |
| `test_apps/unit/` | On-target Unity suite |
| `test_apps/host/` | MinGW/CTest runner for pure logic |
| `tools/` | Verified release packaging, publication and personal USB setup |
