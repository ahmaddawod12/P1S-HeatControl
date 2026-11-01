# P1S Heat Controller Design

This is the original controller design. The sensor path and wiring are
superseded by the [DS18B20 migration](2026-11-01-ds18b20-sensor-design.md);
use the repository README for the current hardware and build instructions.

Date: 2026-08-25

Hardware migration updated: 2026-10-11 (classic ESP32 Dev Kit).

## 1. Purpose

Build a standalone chamber-temperature controller for a Bambu Lab P1S using an ESP32 Dev Kit (ESP32-WROOM-32) and ESP-IDF. The controller measures chamber temperature with a two-wire 10 kΩ B3950 NTC thermistor connected to the ESP32 ADC, switches a 220 VAC / 350 W heater and a 5 V DC fan through separate channels of a two-channel 5 V relay module, and provides a local Wi-Fi web interface.

Version 1 operates independently of the printer. It has no Bambu Cloud, MQTT, printer-job integration, cloud service, or internet dependency.

## 2. User-visible behavior

- The web interface displays current temperature, configured target, operating state, heater state, fan state, Wi-Fi state, uptime, and faults.
- The target is adjustable from 0 to 80 degrees Celsius in 0.5-degree increments.
- A target of 0 degrees Celsius disables heating.
- The remembered default target is 60 degrees Celsius.
- The controller has an explicit Enable/Disable control.
- The selected target persists across restarts, but heating always starts disabled after boot or reset.
- Targets above 60 degrees Celsius display a warning in the web interface.
- There is no application-level password in version 1. Access is limited by the local Wi-Fi network.

## 3. Hardware architecture

### 3.1 Components

- ESP32 Dev Kit (ESP32-WROOM-32) development board
- Two-wire stainless-steel NTC probe, 10 kΩ ±1% at 25 degrees Celsius, Beta 3950 K
- 10 kΩ ±1% fixed divider resistor
- 100 nF ceramic ADC filter capacitor
- Two-channel 5 V relay module with separate relay coils and contact sets
- 220 VAC / 350 W resistive heater
- 5 V DC fan
- Isolated, regulated 220-to-5 V DC supply rated for at least 3 A

The 5 V supply powers the ESP32, relay module, and fan. The ESP32 3.3 V pin powers the NTC divider. The fan rating must fit within the supply budget after allowing for the ESP32 and both energized relay coils.

### 3.2 Relay channels

The relay module contains two independent channels:

- Channel 1 switches the 220 VAC heater circuit through its isolated COM/NO contact set.
- Channel 2 switches the 5 V fan circuit through its separate COM/NO contact set.

The AC and DC contact circuits remain electrically separate. The ESP32 drives the two relay inputs independently. Enabling below the target commands the fan first and the heater second within one controller iteration. The heater runs continuously until the target is reached, then turns off immediately while the fan continues for a 5-second cooldown. An operator disable turns the heater off immediately and keeps the fan on for 10 seconds. A fault turns both automatic outputs off immediately. A manual dashboard request remains an independent fan override. The output layer rejects any request that would energize the heater without the fan.

The relay module must accept 3.3 V input logic. Relay active polarity is a build-time board configuration so the firmware can establish the inactive state immediately during startup. External pull resistors matching that polarity keep both relay inputs inactive before GPIO initialization.

### 3.3 GPIO assignment

| Function | ESP32 GPIO |
|---|---:|
| Heater relay input | GPIO25 |
| Fan relay input | GPIO26 |
| NTC divider midpoint / ADC1 channel 6 | GPIO34 |

GPIO6–11 are reserved for flash. The relay pins avoid boot-strapping pins and the UART0 console on GPIO1 (TX) and GPIO3 (RX), which connects through the board's USB-to-UART bridge. GPIO34 is input-only and uses ADC1 so thermistor sampling remains available while Wi-Fi is running.

The divider orientation is fixed: 3.3 V connects through the 10 kΩ fixed resistor to the GPIO34 junction, and the NTC connects from that junction to ground. A 100 nF capacitor connects from GPIO34 to ground. This orientation makes voltage decrease as temperature rises and keeps the 60–82 degree control region within the ESP32 ADC's calibrated range.

## 4. Firmware architecture

The firmware targets ESP32 using ESP-IDF 6.1 and C. Components have narrow interfaces so temperature acquisition, control logic, and web handling can be tested independently.

The ESP32 ADC uses line-fitting calibration with factory eFuse values when present and a nominal 1100 mV reference otherwise. All firmware images default to 4 MB flash and UART0 logging.

```text
main/
components/
  board/          GPIO assignments, output initialization, relay safety and ordering
  thermistor/     calibrated ADC sampling, divider/B3950 conversion, fault decoding
  temperature/    sampling, filtering, validation, and calibration
  controller/     state machine, threshold logic, and fault handling
  settings/       NVS persistence and validation
  wifi_manager/   SoftAP provisioning and station reconnection
  web_server/     REST API, Server-Sent Events, and embedded web assets
  diagnostics/    runtime status and bounded in-memory fault history
web/
  index.html
  styles.css
  app.js
```

Only the controller component may request output changes. Web handlers update settings or submit controller commands; they never write GPIO directly.

## 5. Temperature acquisition

- Configure GPIO34 / ADC1 channel 6 through the ESP-IDF one-shot ADC driver with 12 dB attenuation and line-fitting calibration.
- Average 16 ADC conversions for each sample and acquire a new sample every 500 milliseconds.
- Convert calibrated junction voltage to NTC resistance with `Rntc = Rfixed * Vjunction / (Vsupply - Vjunction)`.
- Convert resistance with the Beta equation using 10 kΩ at 25 degrees Celsius and Beta 3950 K.
- Apply the configured calibration offset, then use a five-sample median for normal control decisions.
- Evaluate the unfiltered reading as well as the filtered value for over-temperature detection.
- A sample becomes stale if no valid update is available for two seconds.
- Decode ADC rail readings as open- or short-circuit faults and reject temperatures outside the probe's −20 to 105 degree range.
- Require three consecutive valid samples before enabling is allowed.

Any invalid, non-finite, stale, or hardware-faulted measurement forces the heater and automatic fan request off immediately and enters a latched fault state. A separate manual fan request may keep the fan on.

## 6. Controller state machine

```text
BOOT -> PROVISIONING or IDLE
IDLE --enable--> HEATING or HOLDING
HEATING --temperature >= target--> HOLDING
HOLDING --temperature <= target - 1 C--> HEATING
HEATING/HOLDING --disable or target=0--> IDLE
HEATING/HOLDING --fault--> FAULT
FAULT --fault cleared and acknowledged--> IDLE
```

State output rules:

| State | Heater relay | Fan relay |
|---|---|---|
| BOOT | Off | Off |
| PROVISIONING | Off | Off |
| IDLE | Off | 10-second disable cooldown, manual, or off |
| HEATING | On | On |
| HOLDING | Off | 5-second target cooldown, manual, or off |
| FAULT | Off | Manual or off |

Additional rules:

- Enabling requires a target above 0 degrees Celsius and a healthy sensor.
- If enabled while the measured temperature is already at or above the target, the controller enters HOLDING.
- The fixed hysteresis is 1 degree Celsius: outputs turn on at or below `target - 1` and off at the target.
- While heat is demanded, the heater and fan run continuously until the target is reached.
- Reaching the target turns the heater off immediately and keeps the fan on for 5 seconds. A minimum five-second transition interval prevents a normal hysteresis restart from chattering.
- Disable, including setting the target to zero, turns the heater off immediately and keeps the fan on for 10 seconds. A fault turns both automatic output requests off immediately and does not start a cooldown.
- Manual fan off clears only the manual request; active heating and either automatic cooldown override it and keep the fan on.
- A raw or filtered reading at or above 82 degrees Celsius raises a latched over-temperature fault.
- Acknowledging a fault never resumes heating. It returns to IDLE only after the cause has cleared. Sensor faults must remain clear for three consecutive samples; an over-temperature fault requires the chamber reading to fall below 80 degrees Celsius.
- Loss of Wi-Fi does not stop valid local regulation. The controller continues operating and attempts to reconnect.

## 7. Settings and persistence

Stored in NVS:

- Wi-Fi credentials
- Target temperature
- NTC calibration offset

Not stored:

- Enabled/disabled runtime state
- Current controller state
- Active fault latch
- Relay output state

All stored values are validated when loaded. Invalid values revert to defaults. The target default is 60 degrees Celsius and the calibration-offset default is 0 degrees Celsius.

## 8. Wi-Fi provisioning

If no valid station credentials exist, the device starts a SoftAP named `P1S-Heat-XXXX`, where `XXXX` is derived from the device identity. A captive portal scans for local networks, accepts credentials, saves them to NVS, and attempts station-mode connection.

After connecting, the device advertises `p1s-heat.local` with mDNS. It also remains accessible through its DHCP-assigned IPv4 address. A Wi-Fi reset API clears only the network credentials and returns the device to provisioning; it does not erase target or calibration settings.

## 9. Web interface and API

The firmware embeds plain HTML, CSS, and JavaScript. It uses no external CDN or hosted dependency. The page is responsive for phone and desktop browsers.

REST endpoints:

```text
GET  /api/status
PUT  /api/target
POST /api/control/enable
POST /api/control/disable
POST /api/control/fan/enable
POST /api/control/fan/disable
POST /api/fault/acknowledge
POST /api/wifi/reset
GET  /api/events
```

`PUT /api/target` accepts JSON in the form `{"target_c": 60.0}`. The value must be finite, between 0 and 80 inclusive, and aligned to a 0.5-degree increment. A target of 0 disables the controller.

The Server-Sent Events endpoint publishes status at one update per second and immediately after state changes. Status includes:

- Raw, filtered, and calibrated temperature
- Target temperature
- Sensor validity and decoded NTC/ADC faults
- Controller state
- Heater and fan relay states
- Latched fault code and message
- Wi-Fi signal strength and IP address
- Uptime

The web server validates every request independently of browser-side validation.

## 10. Diagnostics and fault model

Fault codes cover:

- NTC open-circuit, short-circuit, out-of-range, and ADC I/O faults
- Invalid or non-finite conversion
- Stale temperature sample
- Software over-temperature
- NVS load or validation failure
- Output initialization failure

The controller logs state transitions and faults through ESP-IDF logging. A bounded in-memory history retains the most recent 32 fault and state-transition records for diagnostics. Historical temperature storage, metrics export, and cloud logging are outside version 1.

## 11. Verification strategy

### 11.1 Host and component tests

- Validate the reversed divider and B3950 conversion against hand-derived 25, 60, and 80 degree reference points.
- Test calibration-offset and median-filter behavior.
- Test every state transition at threshold boundaries.
- Test target validation, including 0, 80, non-finite, out-of-range, and invalid-step values.
- Test stale samples and every thermistor fault mapping.
- Verify faults latch, outputs turn off, and acknowledgement cannot resume heating.
- Test NVS defaults, persistence, corruption fallback, and restart-disabled behavior.
- Test REST request validation and status serialization with mocked controller interfaces.

### 11.2 On-target tests

- Confirm both relay outputs remain inactive during power-up, reset, provisioning, and firmware restart.
- Confirm the relay input polarity and 3.3 V logic compatibility.
- Confirm calibrated GPIO34 ADC readings at ambient and reference temperatures.
- Confirm the fan activates before the heater, the heater runs continuously to the target, and the fan turns off 5 seconds after the target is reached.
- Disconnect and short the probe inputs to confirm immediate fault behavior.
- Interrupt Wi-Fi and confirm local regulation continues.
- Restart while enabled and confirm the target persists but both outputs remain off.

### 11.3 Staged system test

- First test with the relay contacts unloaded.
- Then test the 5 V fan channel without the heater connected.
- Finally integrate the heater and characterize warm-up rate, temperature uniformity, and overshoot at progressively higher targets.

## 12. Acceptance criteria

- The firmware builds for target `esp32` with ESP-IDF 6.1.
- A new device can be provisioned entirely through the temporary Wi-Fi portal.
- The dashboard works locally without internet access.
- A valid 0-to-80-degree target can be saved and restored.
- Heating never resumes automatically after reset or power loss.
- The heater relay is off in every state except HEATING.
- Both channels switch on at or below `target - 1`; at the target the heater switches off immediately and the fan switches off after 5 seconds. Disabling heating switches the heater off immediately and the fan off after 10 seconds.
- Sensor and over-temperature faults switch both automatic output requests off and appear in the web interface.
- Target, state, measurements, relay states, Wi-Fi information, and faults update live in the browser.
- The controller continues valid regulation during a Wi-Fi outage.

## 13. Version 1 non-goals

- Bambu printer or cloud integration
- Automatic print-job detection
- PID or time-proportional heater control
- User-configurable fan schedules or cooldown duration
- Application-level authentication
- Internet-facing access or port forwarding
- OTA firmware updates
- Long-term temperature history
- Physical display or buttons
- Custom PCB or production enclosure
