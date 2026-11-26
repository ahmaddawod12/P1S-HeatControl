# ESP32 Firmware Plan for Firebase

The P1S Heat Controller will send status to Firebase Realtime Database and receive website commands over outbound HTTPS. Its hotspot and HTTP server will run during Wi-Fi setup, pairing, and connection recovery. The on-device runtime will regulate temperature and decide whether commands can execute.

Qualify the firmware on one ESP32 with ESP-IDF 6.1. Use a host stub, a test Firebase project, and a small pairing and command test client to exercise the firmware ahead of the website.

## Pilot timing

| Setting | Starting value |
| --- | --- |
| Status heartbeat | Every 15 seconds, with prompt updates on fault changes and after commands |
| Stale status | 45 seconds after the last successful upload |
| Wi-Fi recovery | Serve the Wi-Fi form while disconnected and retry the saved network in the background |
| Pairing | Show the pairing code and let the user change networks if enrollment stalls. Temporary Firebase errors retry in the background. |
| Setup shutdown | Close HTTP and the hotspot when the connected controller has a confirmed enrollment |
| Command validity | Up to 30 seconds from issuance. This bounds when a command can start, including commands that launch longer operations. |

## 1. Enroll the device and maintain its Firebase session

- [ ] Add a Firebase client component with project configuration, bounded HTTPS requests, certificate verification, and a clock for TLS and command expiry. Reuse the certificate bundle and review the existing clock service for sharing time synchronization with the signed updater. [Clock service](../../../components/clock_service/include/clock_service.h)
- [ ] After the ESP32 joins Wi-Fi in setup mode, request a short-lived pairing session from the backend. Display a one-time code and prompt the user to return to an internet-connected network to claim it. Once the signed-in owner claims the code on the hosted website, the ESP32 uses its private pairing session secret to fetch a short-lived custom token for its Firebase Auth UID. Exchange the custom token directly with Firebase Auth.
- [ ] Persist the UID and revocable refresh token together in a versioned NVS record. Keep the one-hour ID token in memory and refresh on boot and before expiry. Persist a replacement refresh token when Firebase rotates it. Keep credential values out of the page, logs, and status records. Qualify protected NVS storage against the ESP32 flash layout and signed update process. [Firebase Auth REST API](https://firebase.google.com/docs/reference/rest/auth/) · [ESP-IDF NVS encryption](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_encryption.html) · [Partition layout](../../../partitions.csv)
- [ ] On a timeout, DNS failure, or temporary response from Firebase, retain the session and retry with backoff. When Firebase explicitly rejects the refresh credential, show that the controller needs to pair again. The backend verifies the existing owner's authorization before restoring access to an enrolled device.

## 2. Upload status and track Firebase readiness

- [ ] Publish a versioned snapshot at `/devices/{deviceUid}/latest`. Reuse the runtime and printer snapshots for chamber temperature and validity, target, heater state and mode, fault, printer state, and monitoring status. Include the controller name, firmware version, update progress, command session ID, and a Firebase server timestamp. Omit the numeric temperature when the reading is invalid. [Runtime snapshot](../../../components/runtime/include/app_runtime.h) · [Printer snapshot](../../../components/printer_control/include/printer_control.h) · [Firebase server values](https://firebase.google.com/docs/reference/rest/database#server-values)
- [ ] Run Firebase I/O in background workers. Use bounded timeouts and retry with backoff. After reconnecting, send the latest snapshot. Publish changes in command readiness so the website can tell whether a controller with a recent reading can also receive commands.
- [ ] Signal when an authenticated upload succeeds and the command stream subscribes successfully. Send connection health changes to the Wi-Fi recovery manager. The heater runtime regulates independently of either cloud connection.

## 3. Receive and acknowledge website commands

| Website action | Firmware service |
| --- | --- |
| Set target, enable heat, disable heat, acknowledge fault | Existing `app_runtime` APIs |
| Set sensor calibration | Existing calibration API and persistence |
| Start or stop Monitor P1S | Existing `printer_control` APIs. Stopping monitoring keeps manual heating enabled. |
| Save or forget printer settings | Existing asynchronous printer configuration worker |
| Start a signed firmware update | Existing `firmware_update` worker and maintenance checks |

- [ ] Subscribe to the device command path over the Realtime Database HTTPS event stream. Handle the initial snapshot, incremental updates, redirects, disconnects, and token renewal. Bound stream memory and reconnect on a stalled connection. Refreshing a short-lived ID token is part of regular operation. [Realtime Database REST streaming](https://firebase.google.com/docs/reference/rest/database#section-streaming)
- [ ] Require a supported command and payload, unique ID, sequence, current session ID, and server-issued expiry. Publish a new session ID after boot or a confirmed loss of command connectivity. Accept commands only when the clock can verify expiry. Reject expired requests. An unexpired Disable may use a previous stream session from the same boot and controller UID. Deduplicate by ID, recheck Wi-Fi connectivity and its generation before an Enable takes effect, and check the remaining deadline before applying heat.
- [ ] Serialize dispatch through the existing runtime and service admission checks. Record the last dispatched sequence so a delayed older request cannot override it. A valid Disable heat supersedes older pending requests that could enable heating. Apply the existing input bounds, fault lockouts, printer permit checks, and update maintenance rules. Drive the relay through the runtime. [Command dispatch](../../../components/firebase_client/firebase_commands.c) · [Runtime API](../../../components/runtime/include/app_runtime.h)
- [ ] Write a result keyed by command ID. Use `accepted` for work admitted to an asynchronous service and `applied`, `rejected`, or `expired` when the outcome is known. Include a stable rejection reason when applicable. For Disable heat, confirm the off state in the runtime before reporting `applied`. The website can show an unconfirmed result when the device has yet to respond.
- [ ] Persist the signed update's command ID through reboot so the controller can report its outcome. For printer configuration, use a command with a short-lived reference to the access code, fetched over device-authenticated HTTPS. Store the code through the existing settings API and redact printer credentials from results. [Printer configuration API](../../../components/printer_control/include/printer_control.h)

## 4. Replace the OTA boot probe

- [ ] Replace the HTTP probe and connected-network requirement with on-device readiness checks. Verify safe outputs, accessible settings, fresh runtime and sensor data, and initialization of recovery and command services. Keep the watchdog, signature checks, timeout, and rollback decision for an unhealthy local boot. [Current trial health evaluation](../../../components/firmware_update/firmware_trial.c)
- [ ] Test an upgrade across the current OTA slot layout. Verify that a healthy image confirms during a Firebase outage, then resumes uploads and command listening when access returns. Preserve existing Wi-Fi, printer, and heater settings through this upgrade. Measure signed image size against both slots and check heap and stack headroom with the Firebase stream, telemetry, and printer link active. [OTA release guide](../../ota-release-guide.md)

## 5. Limit the local HTTP server to setup and recovery

- [x] Use the Wi-Fi worker to coordinate station reconnects and hotspot and HTTP lifecycle. Disconnects open Wi-Fi setup. A station connection switches to pairing when required. [Wi-Fi engine](../../../components/wifi_manager/wifi_engine.c)
- [x] Bind local HTTP to the password-protected hotspot interface. Serve the Wi-Fi form and credential endpoints while disconnected or awaiting enrollment. Serve the pairing page and code while connected and unpaired. [Setup routes](../../../components/web_server/setup_portal.c)
- [x] Close the setup portal and hotspot once the connected controller is paired. A Firebase outage leaves setup closed while Wi-Fi remains connected. [Wi-Fi worker](../../../components/wifi_manager/wifi_manager.c)
- [x] Start Firebase from device startup and service retries. Build the local firmware with setup routes only. An image without Firebase enabled keeps the relay off and starts no network services. [Application startup](../../../main/app_main.c)

## Firmware contract for the website plan

| Firebase record | Access and purpose |
| --- | --- |
| `/devices/{deviceUid}/latest` | The controller writes its snapshot. Its paired owner reads it. |
| `/devices/{deviceUid}/commands/{commandId}` | A trusted callable function checks ownership and writes a validated command. The addressed controller subscribes to its own commands. |
| `/devices/{deviceUid}/results/{commandId}` | The controller writes its result. Its paired owner reads it. |
| `/owners/{ownerUid}/devices/{deviceUid}` | The pairing backend maintains the association. The signed-in owner lists their controllers. |

A command contains `id`, `sequence`, `sessionId`, `type`, `payload`, `issuedAt`, and `expiresAt`. A result carries the command ID, status, reason when applicable, and timestamp. The backend uses the current session and last-seen time when admitting actions that can start heating. It expires and prunes old command records so a reconnect reads a bounded queue. Database rules use the authenticated UID for device access and the ownership mapping for dashboard access. [Realtime Database Security Rules](https://firebase.google.com/docs/database/security/rules-conditions)

### Pilot build configuration

Enable `CONFIG_P1S_FIREBASE_ENABLED` in an ignored SDK configuration for the pilot build. Set `CONFIG_P1S_FIREBASE_DATABASE_URL` to the HTTPS database origin, `CONFIG_P1S_FIREBASE_API_KEY` to the Firebase web API key, and `CONFIG_P1S_FIREBASE_PAIRING_URL` to the HTTPS enrollment base URL. Set `CONFIG_P1S_FIREBASE_SETUP_PASSPHRASE` to a unique 8–63 character hotspot password for the test controller. Build with the selected configuration using the ESP-IDF `SDKCONFIG` option.

For a pilot that starts with Wi-Fi setup, set `CONFIG_P1S_WIFI_SSID=""` and `CONFIG_P1S_WIFI_PASSWORD=""` in that file and build with `-D P1S_BOOTSTRAP_CREDENTIALS=ON`. Flash the build through the USB-to-UART adapter. [Direct USB flash](../../ota-release-guide.md#one-time-usb-setup)

The Firebase build starts the portal at `192.168.4.1` over the setup hotspot. Its firmware exchanges Firebase tokens over HTTPS and stores the refresh credential in the existing device configuration partition. Production use requires qualifying encrypted credential storage and the signed release profile.

### Pairing endpoints implemented by the backend

| Request | Response |
| --- | --- |
| `POST {pairingUrl}/begin` with `deviceUid` (empty on first enrollment) and `recoveryKey` for an existing UID | `200` with `code`, `sessionSecret`, and numeric `expiresIn` in seconds, up to 900 |
| `POST {pairingUrl}/poll` with `sessionSecret` | `202` while waiting, `410` after expiry, or `200` with `deviceUid` and `customToken` after an authorized claim |
| `POST {pairingUrl}/complete` with `idToken` and `recoveryKey` | `204` after the authenticated controller confirms that it has stored its credentials |

Generate the display code and session secret independently. Use at most 16 letters, numbers, underscores, or hyphens for the display code, at least 32 and at most 128 of those characters for the secret, and at most 128 for the device UID. The backend signs the Firebase custom token for the paired UID. Allow polling retries within the claimed session's lifetime so a lost response can be recovered, and rate-limit code claims.

Store pending confirmation with the refresh credential and retry confirmation after a reboot if needed. The backend makes the controller visible to its owner on confirmation.

Generate the 32-byte recovery key on the controller, encode it as 64 lowercase
hex characters, and persist it with the refresh credential before confirmation.
The backend stores its hash and verifies the key before issuing a code for an
existing UID. Controllers with older enrollment records register their key after
a successful refresh.

### Command types in the pilot firmware

| Type | Payload |
| --- | --- |
| `heat.setTarget` | `targetC` |
| `heat.enable`, `heat.disable`, `heat.ackFault` | Empty |
| `sensor.setCalibration` | `calibrationOffsetC` |
| `printer.monitorStart`, `printer.monitorStop` | Empty |

Use integer epoch milliseconds for `issuedAt` and `expiresAt`. Give every immutable command a unique ID of at most 64 letters, numbers, underscores, or hyphens. Use a positive, increasing 32-bit sequence per device. Read `sessionId` and `commandReady` from the latest snapshot before creating the command. The firmware reports `applied`, `rejected`, or `expired` when it can confirm an outcome. A missing result remains unconfirmed in the website. Keep the live command queue small enough to fit the 6 KiB event buffer.

Remote printer configuration and firmware update dispatch are later migration steps. Keep the Firebase profile on a bench controller until the pairing service, database rules, and device behavior are verified against the test project.

## Firmware acceptance checks

- [ ] Verify Wi-Fi setup on first boot and router disconnect. Verify the switch to pairing after Wi-Fi connects and the hotspot shutdown after pairing. Test a Firebase outage with Wi-Fi connected and confirm setup stays closed.
- [ ] Run host and on-device tests for saved settings, pairing, token renewal, credential revocation, and reboot with valid stored enrollment. Use two device identities and two owner accounts to test forbidden reads and writes against the Firebase access rules.
- [ ] Test malformed commands, expiry while queued, out-of-order and duplicate delivery, a lost acknowledgement, and old commands arriving after reconnect. Verify fault lockout and that Disable heat reports the actual controller result.
- [ ] On a bench ESP32 with relay contacts unloaded, exercise the full command loop, local regulation during network loss, healthy signed OTA, and rollback on an unhealthy boot. Follow the existing commissioning sequence before connecting the heater. [Host tests](../../../README.md#host-tests-without-a-board) · [Commissioning checklist](../../commissioning-checklist.md)
