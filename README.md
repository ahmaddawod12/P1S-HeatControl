# P1S Chamber Heat Controller

ESP-IDF firmware for an ESP32 Dev Kit (ESP32-WROOM-32) that reads a three-wire
DS18B20 digital temperature probe over 1-Wire and controls a 220 VAC / 350 W
heater and a 5 V fan together with one relay. Live telemetry and operator
commands use the signed-in Firebase-hosted dashboard.

The firmware targets the classic `esp32` chip with ESP-IDF v6.1 and 4 MB flash.
The USB-to-UART adapter connects to UART0 for flashing and the serial console.
The DS18B20 DATA input uses bidirectional GPIO27. GPIO16 controls the shared
heater and fan relay.

**Monitor P1S** uses verified local MQTT to read the printer state and turns
manual heating off when the watched print ends. Live monitoring trials are
still pending.

The controller pairs with a dashboard owner through Firebase. Its local HTTP
service is limited to Wi-Fi setup and pairing on its recovery hotspot.
[Firebase dashboard setup and verification](cloud/README.md)

> **Mains warning.** The heater uses 220 VAC. Commission the board
> with the relay contacts unloaded and work through
> [docs/commissioning-checklist.md](docs/commissioning-checklist.md) in order.
> Do not connect the heater until every earlier stage records a pass.

## Hardware

| Signal | GPIO | Notes |
|---|---|---|
| Heater and fan relay | GPIO16 | Switches both loads together |
| DS18B20 DATA | GPIO27 | Digital 1-Wire; pull up to 3.3 V through 4.7 kΩ |

When replacing the NTC, disconnect its divider and filter capacitor. Connect
DS18B20 DATA to GPIO27, VCC to 3.3 V and GND to ESP32 GND. GPIO34 is input-only
and cannot drive 1-Wire. Connect the shared relay input to GPIO16. Use the
GPIO labels on the board, not the header
positions. GPIO6–11 are used by flash.
See [Espressif's DevKitC pin table](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html).

The heater and circulation fan run together until the chamber reaches its
target. Reaching the target, disabling heating, a monitoring hold or a latched
fault switches both off. At boot the relay GPIO is pulled to its inactive level
and the output register is preloaded before the driver is enabled.

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
One successful read resets the failure count. Failure 5 latches the sensor fault
and switches off the heater and fan.
If the acquisition task stops completing reads, the independent two-second
stale-task timeout still shuts heating down, including a stall after a bad read.
A gap over two seconds clears the old filter and valid-sample streak. Heating
shuts down immediately if the unadjusted probe reading, latest calibrated
reading, or filtered temperature reaches 82 °C. During retries without a usable
reading, heating can continue briefly without fresh temperature feedback.
Reconnect the probe, wait for three clean samples, then acknowledge a latched
fault; heating remains disabled until explicitly enabled. An enabled heater
that is already off requires three clean samples and the five-second restart
interval before regulation can turn it on again.
See the [DS18B20 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/DS18B20.pdf)
and [migration design](docs/superpowers/specs/2026-11-01-ds18b20-sensor-design.md).

### Relay polarity

The current configuration is **active high**: `false` (OFF) drives LOW (`0`),
and `true` (ON) drives HIGH (`1`). The shared relay on GPIO16 initializes LOW.

For active-low relay inputs, enable the polarity option with:

```powershell
idf.py menuconfig
```

under **P1S Heat Controller Board → The heater and fan relay is active low**
(`CONFIG_P1S_RELAY_ACTIVE_LOW`).

## Control behaviour

| Setting | Value |
|---|---|
| Target range | 0 to 80 °C in 0.5 °C steps |
| Default target | 60 °C |
| Target 0 | Disables heating |
| Restart heating at | On at or below the selected restart temperature, off at the target. Choose 0.5 to 20 °C below the target in half-degree steps. The saved gap follows target changes and defaults to 1 °C. |
| Heater operation | Manual Enable can start below the target. Once heating starts, it runs continuously until the target or a shutdown condition is reached. |
| Minimum restart interval | 5 s after shutoff, including a quick Disable and Enable. Target and safety shutoffs are immediate. |
| Circulation fan | Switches on and off with the heater |
| Over-temperature | Latched fault at 82 °C from the unadjusted probe, calibrated, or filtered temperature |

Wi-Fi credentials, target, restart temperature, calibration offset, and optional
printer connection settings are saved in flash. The relay starts inactive and
stays off for at least five seconds on every boot. An interrupted heating
session or a saved safety fault leaves heating locked out after restart.
Wait for three clean probe samples and readings below 80 °C, acknowledge the
lockout, and then select Enable heat. A pending cloud Disable blocks any new
relay start.

Acknowledging a fault returns the controller to IDLE with heating disabled.
Sensor faults need three consecutive clean samples. An over-temperature fault
requires the unadjusted, calibrated, and filtered readings to be below 80 °C.
An output-initialisation fault requires a restart.

Regulation keeps running through a Wi-Fi outage.

### Optional Monitor P1S

For a previously configured printer, use **Monitor P1S** on the hosted
dashboard. The firmware preserves its saved IPv4, serial and LAN access code.
The ESP32 connects to the printer's local MQTT over TLS and verifies the
printer certificate against the Bambu CA and the
serial. The printer stays in its normal cloud mode: no LAN-only mode, Developer
Mode or time server is needed, and Bambu Handy and cloud printing keep working.
An unavailable printer does not block manual chamber control or dashboard
startup.

Use the unloaded-relay checks before attempting heated integration.
[Commissioning checklist](docs/commissioning-checklist.md)

The dashboard provides **Enable heat**, **Start Monitor P1S** and **Disable heat**.
Monitoring is available while heating is enabled and the
printer state is known, and turns the heater off by itself when the print ends:

| Printer state when Monitor P1S is pressed | What happens |
|---|---|
| FINISH, FAILED or IDLE (left over from the last print) | Heating continues and waits for the next print to start |
| PREPARE or RUNNING | Heating continues and watches this print |
| PAUSE | Heater and fan held off until the print runs again |

| While monitoring | Heater and fan | Monitoring |
|---|---|---|
| The watched print ends in FINISH or FAILED, or drops to IDLE | Off | Ends; heating disabled |
| RUNNING or PREPARE → PAUSE | Held off | Continues |
| PAUSE → RUNNING | Back on | Continues |
| Connection lost, or no report for 5 minutes while printing | Held off | Continues; heating can return when the printer reports an active state |
| No print starts within 60 minutes of pressing | Off | Ends; heating disabled |

**Stop monitoring (keeps heating)** returns the controller to manual heating.
**Disable heat**, a fault, maintenance, and printer settings changes
end the monitoring session and disable heating.

After a power loss, the controller reconnects using the saved printer settings.
Reboot ends Monitor P1S sessions and leaves heating disabled, including when
the session was waiting for a print. A fresh printer report gives the current
state only, and a print may have finished while the controller was offline.
Use **Enable heat** and **Start Monitor P1S** to start a new session. While
monitoring, the relay needs a renewed 5-second permit.

The ESP32 subscribes to the printer's regular status updates and keeps only
`gcode_state`. After reconnecting, the dashboard shows **Unknown** until an
update includes the print state. While monitoring, the relay stays off during
that wait. Report bursts and unreadable reports leave the connection open.

## Wi-Fi setup and dashboard access

1. Join the controller's password-protected `P1S-Heat-Setup-<board ID>`
   hotspot and open [192.168.4.1](http://192.168.4.1/). Its suffix matches
   the board ID on the setup page and the hosted dashboard.
2. If the controller is disconnected, enter the Wi-Fi network and password.
   Leave the password blank for an open network.
3. If pairing is required after Wi-Fi connects, copy the code on the pairing
   page. Return to a network with internet access, sign in to the hosted
   dashboard and enter the code.
   If a code does not appear, use **Change Wi-Fi network** on the pairing
   page to join a network that can reach Firebase and the time server.
4. When using multiple boards, pair each board from its own hotspot. Select
   its board ID in **Controllers** before sending commands.

| Controller state | Setup hotspot |
| --- | --- |
| Wi-Fi disconnected | Wi-Fi form. The controller keeps retrying its saved network. |
| Wi-Fi connected, pairing required | Pairing code page with a link to change Wi-Fi. |
| Wi-Fi connected, paired | Hotspot and local HTTP are off, including during temporary Firebase outages. |

Saved Wi-Fi settings take precedence over a compiled bootstrap network. The
saved Wi-Fi password stays on the controller. The signed-in dashboard reads
chamber, printer and firmware status and sends supported commands through Firebase.
A confirmed controller keeps regulating locally during network interruptions.
[Dashboard setup and deployment](cloud/README.md)

## Build and flash

Build a configured Firebase image and flash it directly through the USB-to-UART
adapter. A build with Firebase disabled keeps the relay off and starts no
network services. Website-triggered firmware updates and printer configuration
are pending. [Direct USB setup](docs/ota-release-guide.md#one-time-usb-setup)

### Firebase and Wi-Fi configuration

For the hosted dashboard, use the public firmware and
[USB setup instructions](docs/ota-release-guide.md#one-time-usb-setup).

For a separate Firebase project, configure it in an ignored `sdkconfig.bootstrap` before building a device
image. Use the same Firebase project for the firmware and the website, and a
distinct setup hotspot passphrase for each board.
[Firebase firmware build settings](docs/superpowers/plans/2026-11-23-firebase-firmware.md#pilot-build-configuration)

The personal bootstrap can seed a Wi-Fi network from the ignored
`sdkconfig.secret` beside `sdkconfig.defaults`:

```ini
CONFIG_P1S_WIFI_SSID="Your network"
CONFIG_P1S_WIFI_PASSWORD="your-passphrase"
```

Set `P1S_BOOTSTRAP_CREDENTIALS=ON` explicitly in a separate bootstrap build.
Set both Wi-Fi values to empty in the bootstrap configuration to enter the
network on the setup page. A nonempty passphrase must be 8 to 63 bytes.

A compiled-in passphrase is recoverable from the flash image by anyone who can
read the chip. Treat a flashed board as carrying the network password.

Build from an ESP-IDF v6.1 PowerShell environment. The build checks the Firebase
URLs, API key, and setup hotspot password.

```powershell
idf.py -B build-bootstrap -D SDKCONFIG=sdkconfig.bootstrap -D P1S_BOOTSTRAP_CREDENTIALS=ON build
```

Use the [direct flashing command](docs/ota-release-guide.md#one-time-usb-setup)
for the adapter's COM port. To build and run the on-target unit tests instead:

```powershell
idf.py -C test_apps/unit set-target esp32
idf.py -C test_apps/unit build
$p1sPort = 'COMx' # Replace with the explicitly selected board's port.
pytest test_apps/unit/pytest_unit.py --target esp32 --embedded-services esp,idf --port $p1sPort -s
```

`espressif/cjson`, `espressif/ds18b20` (0.4.0) and
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

The pytest runner flashes the Unity test image onto the board. Flash the
configured Firebase firmware again after the suite completes.

### Clean rebuild and full suite

```powershell
idf.py -B build-bootstrap -D SDKCONFIG=sdkconfig.bootstrap -D P1S_BOOTSTRAP_CREDENTIALS=ON build
idf.py -C test_apps/unit fullclean
idf.py -C test_apps/unit set-target esp32
idf.py -C test_apps/unit build
$p1sPort = 'COMx' # Replace with the adapter's port.
pytest test_apps/unit/pytest_unit.py --target esp32 --embedded-services esp,idf --port $p1sPort -s
```

### Sensor smoke test

With the relay contacts unloaded and the DS18B20 adapter connected to GPIO27,
3.3 V and GND as described above:

```powershell
idf.py -C test_apps/sensor_smoke set-target esp32
idf.py -C test_apps/sensor_smoke build
idf.py -C test_apps/sensor_smoke -p $p1sPort flash monitor
```

This image keeps the relay off and checks ten ambient-temperature samples.
Flash the configured Firebase firmware again after testing.

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

## Startup order

`app_main` initializes the relay before starting the other services.

1. `board_outputs_init` drives the shared relay off.
2. OTA trial supervisor — boot maintenance and startup watchdog when unconfirmed
3. `nvs_flash_init` and `settings_init`
4. `app_runtime_start`
5. `wifi_manager_start` — dedicated device configuration and saved Wi-Fi
6. `firebase_client_start` starts telemetry and command handling and registers
   the recovery portal with the Wi-Fi worker.
7. The optional printer coordinator reconnects using the saved settings.
8. The signed update worker starts after startup reports its result.

A relay or runtime failure stops startup with the outputs off. Wi-Fi and
Firebase worker startup failures are retried in the background.

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
| `components/printer_link/` | Print-state updates over a verified local MQTT subscription |
| `components/printer_control/` | Monitor P1S state machine and coordinating worker |
| `components/diagnostics/` | 32-entry transition ring buffer |
| `components/wifi_manager/` | Saved Wi-Fi credentials, reconnects and hotspot lifecycle |
| `components/firebase_client/` | Firebase pairing, telemetry and command handling |
| `components/web_server/` | Hotspot setup portal and its HTTP handlers |
| `components/app_startup/` | Injectable startup order and failure policy |
| `cloud/` | Hosted dashboard, pairing functions and database rules |
| `web/` | Wi-Fi setup and pairing pages embedded in the firmware |
| `test_apps/unit/` | On-target Unity suite |
| `test_apps/host/` | MinGW/CTest runner for pure logic |
| `tools/` | Verified release packaging, publication and personal USB setup |
