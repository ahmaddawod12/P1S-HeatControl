# Wi-Fi change and first printer connection test

This procedure records tests for earlier LAN firmware. Use the hosted
dashboard and setup flow for current builds. [Current setup](../README.md#wi-fi-setup-and-dashboard-access)

Prepared 2026-11-07. Use the network currently connected on the PC, as requested
by the owner. This procedure tests monitoring with the printer idle and the
chamber outputs disabled. USB serial, the network change and the first live
printer monitoring connection have passed. The five-minute observation completed
with intermittent stale telemetry, so continuous freshness is not a pass.

Updated 2026-11-08 for **Monitor P1S** (no time server, no Resume). The evidence
record at the end is historical and was collected with the earlier firmware.

## Before connecting the board

- Have a USB **data** cable and identify the ESP32 Dev Kit / ESP32-WROOM-32.
- Disconnect the relay loads, including the 220 VAC heater. USB power is
  sufficient for this connection test. Leave heating disabled.
- Have the current Wi-Fi name and password available locally. The classic
  ESP32 and P1S need 2.4 GHz. The PC can stay on 5 GHz if both bands share the
  same LAN. Avoid guest/client isolation between the devices. The P1S radio
  specification is in its [official quick start guide](https://cdn1.bambulab.com/documentation/quick-start-59b0cefdc0fc4/P1S/English%20version-Quick%20Start%20Guide%20for%20P1S.pdf).
- Put the printer on that LAN first if necessary. Record its **current IPv4**,
  **full serial**, **LAN access code** and **exact firmware version**. Record
  Bambu Studio's version if installed; Studio is not required for monitoring.
  Do not reset the printer or regenerate its access code.
- Leave the printer in its normal cloud mode. Monitor P1S only reads status,
  so LAN-only mode and Developer Mode are not needed, and Bambu Handy and cloud
  printing keep working.
- Allow ESP32-to-printer TCP **8883** and PC-to-ESP32 TCP **80**. No time
  server is needed: the printer certificate is checked against the Bambu CA and
  the printer serial, not against its dates. These connections do not need
  router port forwarding.

There is no cable from ESP32 to printer: they communicate over Wi-Fi. The PC's
USB connection supplies power and serial observation. A missing DS18B20 can
leave a sensor fault while printer monitoring still works. Record it
separately and leave heating disabled. Sensor and relay commissioning use the
[commissioning checklist](commissioning-checklist.md).

## 1. Identify the board and open the serial monitor

List ports before and after plugging in the board. Select the port belonging
to this ESP32; never choose the first COM port automatically.

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Description
```

Use **115200 baud, 8 data bits, no parity, 1 stop bit, no flow control**. Close
other applications holding the port. On this workstation the installed
ESP-IDF monitor runs directly without building or flashing:

```powershell
$p1sPort = 'COMx' # Replace with the positively identified ESP32 port.
& 'C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe' -m esp_idf_monitor --port $p1sPort --baud 115200 --target esp32 --no-reset --disable-address-decoding --decode-panic disable --decode-coredumps disable --timestamps
```

Exit with **Ctrl+]**. `--no-reset` suppresses the monitor's startup reset; USB
power-up itself still boots the board. If boot finished before the monitor
opened, press **EN** once with relay loads disconnected. BOOT is not needed.
Serial is a log stream; the application has no serial Wi-Fi command prompt.
Do not run `flash`, `erase-flash`, or the Unity pytest runner for this test;
the latter replaces the application with a test image.

Look for the installed firmware/version banner and these messages (order can
differ, and a working station may not need the hotspot):

```text
controller services started
connected as <controller-ip> (rssi <value>)
setup hotspot started at 192.168.4.1
```

Investigate any panic, brownout, reset loop or active output before continuing.
A local build's version does not identify the image installed on the board.

## 2. Change Wi-Fi to the PC's current network

1. Open the existing dashboard if its old network is reachable. Otherwise,
   wait up to 30 seconds for **P1S-Heat-Setup**, join from a phone or PC, and
   open [http://192.168.4.1/](http://192.168.4.1/). Without saved credentials,
   the hotspot starts immediately. Stay connected despite a no-internet notice.
2. In **Wi-Fi settings**, enter the target SSID/password exactly and click
   **Save & Connect** once. Existing saved settings take precedence over
   compiled bootstrap credentials: editing `sdkconfig.secret` alone does not
   change a previously provisioned board.
3. Watch for `connected as <controller-ip>`. The change should not reboot the
   board. The setup hotspot closes about ten seconds after a successful join.
4. Return the phone/PC to the target network. Open the recorded controller IP
   or [http://p1s-heat.local/](http://p1s-heat.local/). If name discovery fails,
   use serial/the router to find the IP. `192.168.4.1` is only the setup address.
5. Confirm heating remains disabled. For rejected credentials,
   return to the hotspot and correct them. A lost browser reply alone does
   not prove failure; inspect serial/current status before retrying Save.

The Wi-Fi panel displays the saved password in plain text. Keep it out of
shared screenshots/logs. Identify the installed image before considering an
upgrade if the required panel or API is absent.

## 3. Configure printer monitoring

With the printer idle, open **Printer connection**:

| Field | Value |
|---|---|
| Monitoring enabled | Checked |
| Printer IPv4 | Current address shown on the printer on the target LAN |
| Serial | Full printer serial, copied exactly |
| LAN access code | Enter locally; this differs from the Wi-Fi password |

Save once. Wait for `applying: false` and `config_result: saved`; a save/202
acknowledgement alone does not prove connection. Allow up to two minutes for
the first connection and retries as an observation window, not a guaranteed
deadline.

The firmware verifies the Bambu CA and printer serial as the certificate name.
It subscribes to `device/<serial>/report` and uses the print-state field from
regular updates. The connection can be online while the print state remains
**Unknown**. Observe the next update that includes the print state before
checking the dashboard value.

Leave **Monitor P1S** unused during this monitoring test; chamber heating stays
disabled.

## 4. Observe for five minutes

Replace the example address with the controller IP from serial. These requests
only read status. Run at most one diagnostic poller alongside the dashboard.

```powershell
$p1sBase = 'http://<controller-ip>'
Invoke-RestMethod -Uri "$p1sBase/api/firmware" -TimeoutSec 5 |
    Select-Object installed, maintenance, trial_pending
Invoke-RestMethod -Uri "$p1sBase/api/status" -TimeoutSec 5 |
    Select-Object state, fault, enabled, heater_on, fan_on, manual_fan_on, ip_addr, uptime_ms
1..60 | ForEach-Object {
    $p1sPrinter = Invoke-RestMethod -Uri "$p1sBase/api/printer" -TimeoutSec 5
    [pscustomobject]@{
        Time = Get-Date -Format 'HH:mm:ss'
        Configured = $p1sPrinter.configured
        MonitoringEnabled = $p1sPrinter.enabled
        Applying = $p1sPrinter.applying
        ConfigResult = $p1sPrinter.config_result
        Connected = $p1sPrinter.connected
        State = $p1sPrinter.printer_state
        Reason = $p1sPrinter.reason
        CanMonitor = $p1sPrinter.can_monitor
        Monitor = $p1sPrinter.monitor.phase
        Reconnects = $p1sPrinter.reconnects
    } | Format-Table -AutoSize
    Start-Sleep -Seconds 5
}
Invoke-RestMethod -Uri "$p1sBase/api/status" -TimeoutSec 5 |
    Select-Object state, fault, enabled, heater_on, fan_on, manual_fan_on, uptime_ms
```

| Observation | Expected / interpretation |
|---|---|
| `configured`, monitoring `enabled` | True; printer monitoring enable is independent of chamber heating enable |
| `connected` | True once the broker connection and report subscription succeed |
| `printer_state` | Agrees with the printer screen; `unknown` until the first status report arrives |
| `reconnects` | Stays steady during a stable run; a rising count means the connection keeps dropping |
| `can_monitor: false`, `reason: heating_off` | Expected while chamber heating is disabled |
| `monitor.phase: off` | Expected throughout this test |
| Chamber `enabled`, `heater_on`, `fan_on`, `manual_fan_on` | All false before/after; also observe unloaded relay indicators during the run |
| Serial and `uptime_ms` | No panic/reset loop; uptime increases |

There are currently no dedicated serial success messages for MQTT subscription
or parsed reports. Use the dashboard/API for success evidence. A known
`printer_state` proves the verified TLS path worked; the API does not expose
detailed TLS errors.

## 5. Recovery and failure isolation

After the observation, press EN once with relay loads disconnected. Verify
that the saved Wi-Fi and printer connections return and that a session already
watching a print ends with heating disabled. Record this separately.
Router-outage and printer-job tests come later.

| Symptom | Next check |
|---|---|
| No serial text | Correct port, data cable/driver, 115200 baud; press EN after opening monitor |
| Cannot open COM port | Close another application holding it |
| No station IP | SSID/password, 2.4 GHz coverage/security, DHCP; use setup hotspot |
| Dashboard works, printer disconnected | Printer IP, serial, access code and client isolation |
| Basic PC-side port check needed | `Test-NetConnection -ComputerName <printer-ip> -Port 8883`; proves only TCP reachability from PC, not ESP32 TLS/authentication |
| Connected but state unknown | Capture flags and firmware; investigate subscription/topic or parser compatibility |
| Settings save conflicts | Wait for the existing Wi-Fi change, OTA/trial maintenance or printer operation; avoid repeated Save clicks |
| Certificate error | Serial spelling and embedded CA compatibility; retain verification |
| `/api/printer` absent/unavailable | Installed firmware and optional printer startup; inspect before planning a signed application update |

Do not capture raw `/api/wifi`: it contains the password. Before sharing raw
printer status, omit `csrf_token`, serial, private addresses and
job names as appropriate. Preserve status flags and event timing.

## Evidence record

| Check | Actual result |
|---|---|
| PC host suite, fresh build `test_apps/host/build-connection-20261107` | 115/115 passed on 2026-11-07 |
| Wi-Fi and printer dashboard suites | 25/25 passed on 2026-11-07 |
| Installed monitor CLI and settings | Help/flags verified; 115200 in release/bootstrap configs |
| Board / selected COM port / installed image | COM9 at 115200; physical EN reset captured ESP32 rev 3.1, 4 MB, ESP-IDF v6.1, app 1.0.0 compiled Nov 7 2026 01:40:23, ELF SHA prefix `40f4a750b`; controller ready and disabled |
| Printer firmware / LAN-developer settings | Discovery advertised P1S/C12 firmware `01.10.00.00`, cloud connection mode and occupied binding; Developer Mode was not independently queried or changed |
| Wi-Fi save and station address | Passed after owner saved the target network: station got a LAN address at 246,982 ms; fallback AP stopped at 257,102 ms; dashboard/API reachable; RSSI about -87 dBm |
| Fallback setup after physical reset | Passed startup observation: `setup hotspot started at 192.168.4.1` at 30,922 ms; DHCP server started on `WIFI_AP_DEF` |
| Temperature during this connection test | Boot logged `state 0 -> 4, fault 2` (fault / sensor_short); API confirms heating, heater, fan and manual fan false; electrical diagnosis and physical output levels not measured |
| Printer identity and saved settings | Discovered serial matched Bambu Studio's selected printer and locally saved access code; Studio config header `02.08.02.61`; code not printed or stored in test evidence |
| Printer port and controller configuration | PC reached printer TCP 8883; controller accepted one config PUT with 202 and reported `config_result: saved` with code present |
| Verified connection / fresh, complete reports | Passed initial live connection: `connected`, `fresh`, `baseline_valid` true; printer reported idle; inactive cycle `ready`, `can_arm: false`, `resume_attempted: false` |
| Five-minute stability and outputs off | Partial, strict freshness check failed: 31 samples over 300.6 s; connected/baseline valid throughout; 28 fresh and 3 stale samples (50, 60, 190 s); no observed reboot; heating/heater/fan/manual fan false and Resume not attempted throughout |
| Reset restores the printer link and keeps the relay off after a watched print | Pending |

These PC tests do not validate the installed board image or a real printer.
Successful monitoring also does not qualify preheat/Resume; use the separate
[printer integration qualification](printer-integration-qualification.md).

Initial live serial evidence on 2026-11-07:

```text
W (58432) wifi:Haven't to connect to a suitable AP now!
W (61282) wifi: station disconnected (reason 201); control continues
```

The PC remained on the requested network. The controller's mDNS name did not
resolve, and its setup SSID was not visible in the initial PC scan. A monitor
RTS-reset request did not produce a boot log. After the owner pressed EN,
startup and fallback AP/DHCP activation were captured successfully. These
observations do not establish which SSID is saved on the board. No firmware
was flashed or erased, no credentials were changed, and no printer command
or chamber enable was sent during this observation.

After the owner completed Wi-Fi setup, read-only Bambu discovery found the
printer on the target LAN. Its identity matched Studio's saved selected
printer. With the owner's request to configure and try the connection, the
saved code was read locally into memory and submitted once to the controller's
printer configuration API with matching Origin and per-boot CSRF token.
Initial reports became fresh and complete without connecting the temperature
sensor. No cloud/developer mode, printer job or thermal settings were changed.
Positive TLS/telemetry evidence does not cover the wrong-CA, wrong-name or
unsynchronized-clock negative cases in the separate qualification matrix.

The completed 31-sample/five-minute observation is stored in the ignored local
file `.superpowers/printer-connection-2026-11-07-monitor.json`. It contains
status flags and timing, without access codes, CSRF/arm tokens, serial, job
names or addresses. Continuous broker connectivity passed, but continuous
telemetry freshness did not: three samples were stale and then recovered.
Signal stayed about -87 dBm. The source of the report gaps remains unconfirmed;
a separate PC-side TLS probe timed out before it could measure report cadence.
No firmware or freshness-threshold change was made to conceal that limitation.

## Unloaded target and manual-pause observation — 2026-11-07

The owner confirmed that the relay board remains connected normally while its
heater/fan loads are disconnected, and requested a target below the sensor
temperature for a functional test. Live status had recovered to a valid sensor
with no latched fault. The target was changed once from 60 to 23.5 degrees C
using the protected target API (204 confirmed); the sensor had warmed from
the earlier 23.125 reading to approximately 24 degrees C when the target was chosen.
The first readback was 24.375 degrees C. Heating and all output requests
remained disabled. The target is a persisted test setting, not the normal
60-degree target.

At the owner's request, the controller was observed while the owner started
and manually paused a job from the printer app. The trace recorded preparing,
running and paused states. During preparation, reports were fresh but the new
job baseline was incomplete; it became complete around 102 seconds into the
watch. Running was seen at 104.3 seconds, paused at 116.0 seconds, and the
fresh, complete paused state persisted for at least ten seconds. The final
snapshot reported 0% progress, valid temperature 23.625 degrees C, target 23.5,
no fault, and all chamber outputs disabled.

The trace is stored locally in the ignored
`.superpowers/printer-manual-pause-2026-11-07.json`. It contains status flags
and timing without job names, credentials, tokens or device identifiers.

This passed job/pause monitoring, not preheat or automatic Resume. The cycle
stayed inactive (`ready`), `reason` was `qualification_required`, `can_arm`
was false and `resume_attempted` was false. No heat enable, cycle arm or
Resume command was sent. An app pause after starting the job does not prove
that preparation purge/priming/extrusion was prevented; 0% progress is not that
proof. Those observations used the older image with the build gate. The owner
subsequently requested its removal; updated source includes Preheat/Resume in
every build, with the connection/job/chamber checks still enforced. Startup
pause evidence remains part of live workflow validation.

## Signed firmware upload without reset — 2026-11-07

At the owner's request, a no-reset bootloader probe identified the expected
ESP32 on COM9 before upload. Serial-read errors prompted an owner power cycle
and a retry at the requested 115200 baud. The final tool session backed up
settings/boot metadata and the application sectors to be changed, then wrote
the newly signed application only to `ota_0` at `0x20000`.

Flash digest verification and full byte-for-byte readback passed for the
1,245,172-byte image, SHA-256
`2ce9002b0eb6b511f2e882e8f9d5176b2eef29d53bb0b0415e04d6c9deb609bd`.
The boot/settings prefix matched its backup afterward, including saved Wi-Fi,
printer configuration and OTA selection. Short, individually checked read
transactions required seven retries; they recovered without a reset.

The tool closed the serial port with the board left in the RAM flasher
stub/download mode. No tool-issued reset or new application startup occurred.
The local upload record is
`.superpowers/printer-resume-upload-20261107/flash-verification.json`; its
private backups remain ignored. First boot was subsequently observed as
recorded below; live Preheat/Resume testing remains pending.

## Post-upload readiness check — 2026-11-07

After the owner left download mode, the application served the dashboard APIs
and restored its saved Wi-Fi/printer configuration. Firmware status reported
`trial_pending: false`; printer status was connected, fresh and complete, and
the temperature sensor was valid and fault-free. Chamber heating and all
output requests stayed disabled. No controller Resume attempt occurred.

The job was already paused when the ESP32 connected. It remained blocked by
`identity_required`, including after a full five-minute recovery interval
from the first connected observation. A brief running-then-paused transition
was observed without a controller command. The same-run identity rule needs
either a usable job start timestamp or an observed idle/terminal-to-new-job
transition on this connection; merely resuming and pausing the old run does
not supply that history. Start the next test job while the controller is
already connected and observing idle, then pause at startup.

The target is still 23.5 degrees C; the sensor warmed to approximately 25.875
degrees C, so a later unloaded test also needs a target chosen against its
then-current reading for the one-degree/30-second settling requirement.
Sanitized read-only observations are stored beside the upload record in
`post-boot-readiness.json` and `post-boot-refresh-readiness.json`. This passes
post-upload boot/monitoring checks and does not establish automatic Resume.

## Paused-state threshold firmware uploaded — 2026-11-07

The owner superseded the historical identity-history and settling requirements
above: a live, fresh paused printer is sufficient; at/above the captured target
sends one immediate Resume, otherwise chamber control waits until target.
The initial-pause checkbox and optional job-metadata gates are removed.

The new source passed 147 host, 43 browser and 19 packaging tests. Release and
Unity images compiled; Unity was not run on the board. The signed application
was uploaded without reset at 115200 baud to `ota_0` at `0x20000`. Its flash
digest and complete readback passed: 1,245,172 bytes, SHA-256
`2391afb63e25e0080b5952f7641292d3babbd9bdec5d4395adb2266a67c5c0a6`.
Settings/boot metadata matched the pre-write backup. Four read-integrity retries
recovered without resetting. The board was left in download mode, application
not started. Evidence:
`.superpowers/sdd/2026-11-07-paused-target-resume/flash-verification.json`.

The saved unloaded-test target was set to 24.5 degrees C before upload, below
the then-valid 25.375-degree reading. No cycle was armed or Resume sent. After
the owner's next manual boot, verify live paused eligibility and a fresh sensor
reading before exercising the button with heater/fan loads disconnected.
Physical Resume and the separately reported intermittent download stall remain
unqualified; successful software tests and upload do not establish those results.

## Firmware review fixes uploaded without reset — 2026-11-08

At the owner's explicit request, the latest reviewed firmware was signed and
uploaded on COM9 at 115200 baud without a board reset. The initial no-reset
probe needed a retry; the expected ESP32 was identified before any flash write.
Settings/boot metadata and the application sectors were backed up first.
Only the selected `ota_0` application at `0x20000` was written.

The 1,245,172-byte signed image passed native digest verification and complete
byte-for-byte readback. Its SHA-256 is
`af3f1e30a68adb30724342bd2f42b5dc84665016ea2c2317571dd5ddb91f42ba`.
The full boot/settings prefix matched its pre-write backup, including NVS,
printer/Wi-Fi configuration, partition table and OTA selection. Serial read
integrity retries recovered through no-reset reconnections.

The connection was closed with the board in RAM flasher/download mode.
No reset or application-start command was issued; application behavior after
boot has not been checked. Private backups and the verification record remain
local under `.superpowers/firmware-review-upload-20261108/`.

## Verified no-reset re-upload to the original board — 2026-11-08

After connection retries and the owner's connection checks, the original
ESP32 (`7c:d5:44:31:8e:fc`) again responded on COM9. The same signed firmware
was re-uploaded at 115200 baud to the selected `ota_0` slot at `0x20000`.
Native digest verification and complete readback passed for all 1,245,172
bytes, SHA-256
`af3f1e30a68adb30724342bd2f42b5dc84665016ea2c2317571dd5ddb91f42ba`.
The settings/boot-metadata prefix matched its fresh pre-write backup.
No reset or application-start command was issued; the board was left in
RAM flasher/download mode. Evidence and private backups are under
`.superpowers/firmware-review-reupload-20261108/`.

A different ESP32 (`48:af:f3:67:35:48`) had briefly answered earlier checks;
its upload attempts failed during connection and did not write flash.
