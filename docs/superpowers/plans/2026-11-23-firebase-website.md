# Firebase Website Plan for the P1S Heat Controller

Build a signed-in website that pairs with an ESP32, shows its live status, and sends commands to it through Firebase. Use the firmware's existing HTTPS pairing endpoints and Realtime Database paths. During regular operation the browser and ESP32 connect independently to Firebase. [Firmware contract](2026-11-23-firebase-firmware.md#firmware-contract-for-the-website-plan)

Start with one controller and one manually approved password account. Implement the backend and rules alongside the website so the pilot can pair and run commands end to end.

The dashboard, pairing service, database rules, and command function are implemented under `cloud/`. Local verification covers email and password sign-in, firmware-format pairing and telemetry, cross-account isolation, and command results. Hosted preview and bench verification depend on a Firebase test project and an ESP32 pilot board. [Build, test, and deployment instructions](../../../cloud/README.md)

## 1. Set up the hosted app and test environment

- [x] Create a `cloud/` workspace with the static dashboard, Firebase Functions, Realtime Database rules, and emulator configuration. Use the Firebase Web SDK with TypeScript and a static build for Firebase Hosting.
- [x] Require an administrator-created email and password account with `owners/{ownerUid}/enabled` set to `true`. Use Firebase Auth UIDs for owners and separately provisioned Firebase Auth UIDs for controllers. On sign-out or account change, remove listeners and clear the previous account's controller state.
- [x] Develop against the Auth, Realtime Database, Functions, and Hosting emulators. [Firebase Local Emulator Suite](https://firebase.google.com/docs/emulator-suite)
- [ ] Enable Email/Password and add an owner in a separate Firebase test project for the ESP32 and Hosting preview. Deploying Cloud Functions requires the Blaze plan. [Functions deployment](https://firebase.google.com/docs/functions/get-started)

## 2. Implement pairing and ownership

| Caller | Endpoint | Contract |
| --- | --- | --- |
| ESP32 | `POST /api/pairing/begin` | Accept an empty `deviceUid` for first enrollment. Verify the saved `recoveryKey` when an existing UID is supplied. Return HTTP 200 with `code`, `sessionSecret`, and numeric `expiresIn` in seconds. |
| Approved password account | `claimPairing({code})` callable | Claim the code for the current owner and return the assigned controller UID. |
| ESP32 | `POST /api/pairing/poll` | Accept `sessionSecret`. Return 202 while waiting, 410 when expired, or 200 with `deviceUid` and `customToken` once claimed. |
| ESP32 | `POST /api/pairing/complete` | Verify the controller's `idToken`, register the hash of its `recoveryKey`, add it to its owner's list, and return 204. |

- [x] Use an HTTP function for the ESP32 endpoints and a callable function for browser claims. Place the pairing rewrite ahead of the dashboard catch-all in Firebase Hosting. [Firebase callable functions](https://firebase.google.com/docs/functions/callable) · [Hosting rewrites](https://firebase.google.com/docs/hosting/full-config#rewrites)
- [x] Generate an eight-character display code and an independent random session secret with a five-minute lifetime. Store hashes for lookups, rate-limit both pairing requests and code attempts, and expire unused sessions. Follow the character and length limits in the firmware contract. Return the secret only to the ESP32 at session creation.
- [x] Claim the pairing session transactionally so two accounts cannot claim the same code. Give the first enrollment a stable UID. A retry by the same owner returns the same controller. Re-pairing a known controller requires the existing owner's account and keeps the same UID. Commit ownership before the poll endpoint can mint a custom token. Allow the ESP32 to retry a claimed poll until the session expires. [Firebase custom tokens](https://firebase.google.com/docs/auth/admin/create-custom-tokens)
- [x] Guide the user from the Wi-Fi setup hotspot to the hosted site. After they enter the code, show **Waiting for controller** until the first authenticated snapshot arrives.

## 3. Apply Realtime Database permissions

| Path | Read access | Write access |
| --- | --- | --- |
| `/owners/{ownerUid}/enabled` | Admin backend | Project administrator |
| `/owners/{ownerUid}/devices` | That approved owner | Pairing backend |
| `/devices/{deviceUid}/latest` | Paired owner | That active controller |
| `/devices/{deviceUid}/disableRevision` | Paired owner | Command function |
| `/devices/{deviceUid}/commands` | That active controller | Command function |
| `/devices/{deviceUid}/results/{commandId}` | Paired owner | That active controller |
| Pairing sessions, device registry, command receipts and counters | Backend | Backend |

- [x] Deny access by default. Authorize device requests using the authenticated UID and an active device registry entry. Authorize browser reads through the ownership mapping. Keep root and cross-device reads denied.
- [x] Validate telemetry and result shape, size, and allowed fields. Link results to the issued command using its receipt, which outlives the queue entry. Permit retries of the same outcome when only the server timestamp changes. The callable functions validate their own writes and check ownership on every request because Admin SDK writes use backend privileges. [Realtime Database Security Rules](https://firebase.google.com/docs/database/security/rules-conditions)

## 4. Build the dashboard

- [x] Show controllers owned by the signed-in account. Subscribe to each displayed controller's `latest` record and decode `schemaVersion: 1`. Show the controller name, temperature and target, heater output, fault, printer state, Monitor P1S phase, and firmware version and update progress. Use the firmware enums for labels and show an unavailable reading when `temperatureValid` is false. [Telemetry format](../../../components/firebase_client/firebase_client.c) · [Printer states](../../../components/app_types/include/printer_types.h) · [Firmware update states](../../../components/app_types/include/firmware_types.h)
- [x] Calculate age from Firebase's `updatedAt` server timestamp. Show **Waiting for first update** for a newly paired device and **Last seen** alongside stale readings after 45 seconds. Offer controls when the browser is connected and the device has a fresh snapshot, `commandReady: true`, and a current `sessionId`. Update the stale indicator even when Firebase sends no new snapshot.
- [x] Build responsive controls for the current firmware actions. Keep **Disable heat** available while another command is pending whenever the command connection is healthy. Label **Stop monitoring (keeps heating)** and show current heater output from telemetry. Show precondition failures from the device, including faults and maintenance.

| Action | Command type | Payload |
| --- | --- | --- |
| Set target | `heat.setTarget` | `targetC` from 0 to 80 °C in 0.5 °C steps |
| Enable or disable heat | `heat.enable` or `heat.disable` | Empty object |
| Acknowledge fault | `heat.ackFault` | Empty object |
| Set sensor calibration | `sensor.setCalibration` | `calibrationOffsetC` from −10 to +10 °C |
| Start or stop Monitor P1S | `printer.monitorStart` or `printer.monitorStop` | Empty object |

## 5. Create commands and show device confirmations

- [x] Add `issueCommand({deviceUid, type, payload, requestId, sessionId, disableRevision})`. Verify the owner, active device, allowed action, and exact payload on the backend. Read the latest snapshot and check its age and command readiness. Require the session shown at submission for new commands other than Disable. Derive timestamps and expiry on the server. Capture the controller's current Disable revision when submitting an action. A missing revision starts at zero, and accepting Disable advances it in the same transaction that clears older commands. Requests still in flight with an earlier revision are refused.
- [x] In a per-device transaction, bind the command to the fresh snapshot's session, allocate a positive 32-bit sequence, and create one immutable command record. Use the browser's request UUID as the command ID and keep a private receipt so retrying the same request returns the original submission without requeuing it. Reject attempts to reuse that ID with different input. Write `id`, `sequence`, `sessionId`, `type`, `payload`, `issuedAt`, and `expiresAt` using the exact firmware schema. Expire commands after 30 seconds. [Firmware command parser](../../../components/firebase_client/firebase_protocol.c) · [Realtime Database transactions](https://firebase.google.com/docs/database/web/read-and-write#save_data_as_transactions)
- [x] Prune expired queue entries and cap the live queue at eight records, reserving space for Disable heat. Keep the stream payload below the ESP32's 6 KiB frame limit. Retain results and receipts for a bounded period so the site can recover a delayed acknowledgement after reconnecting.
- [x] After submission, listen to `/devices/{deviceUid}/results/{commandId}` and read its `id`, `status`, `reason`, and `timestamp`. Show **Pending** while waiting, then **Applied**, **Rejected**, or **Expired** when the controller reports an outcome. A timeout or lost response becomes **Unconfirmed**. Update the command if a late result arrives. Keep the displayed heater state tied to the latest reported telemetry. [Device result handling](../../../components/firebase_client/firebase_commands.c)

## 6. Verify and deploy the pilot

- [x] Exercise pairing in the emulators with two owners. Verify one-time claims, simultaneous attempts, expiry, polling retries, and re-pairing with the original owner. Use two device identities to check reads and writes across owners, controllers, command records, and results. [Firebase rules testing](https://firebase.google.com/docs/rules/unit-tests)
- [x] Test command validation, retry deduplication, concurrent sequence allocation, stale and disconnected devices, late results, and the reserved Disable heat slot. Check that the dashboard distinguishes a confirmed Disable from one whose result is unconfirmed.
- [ ] Deploy rules and functions to the test project and preview the dashboard there. Set the firmware's pairing base URL to that HTTPS endpoint. With the ESP32 relay contacts unloaded, pair using the real setup code, confirm the hotspot closes, verify telemetry and commands, and test reconnect after a Firebase outage. Confirm old-session and expired commands cannot enable heating. Publish the dashboard once the one-device pilot passes. [Firebase Hosting previews](https://firebase.google.com/docs/hosting/test-preview-deploy) · [Controller commissioning](../../commissioning-checklist.md)

## Management actions after firmware support

- [ ] Add printer configuration and forgetting after firmware supports the actions. Send the printer access code through the protected device-authenticated exchange defined in the firmware plan. Show the result of the configuration save.
- [ ] Add **Update firmware** after cloud update dispatch and a signed Firebase-compatible release are available. Show device-reported progress and the confirmed version or rollback outcome after reboot. [Firmware migration plan](2026-11-23-firebase-firmware.md#3-receive-and-acknowledge-website-commands)
