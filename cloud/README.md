# P1S Heat Control Dashboard

The dashboard uses manually approved Firebase email and password accounts. Owners can pair ESP32 controllers, view their latest telemetry, and send heater, target, calibration, fault, and Monitor P1S commands. The device publishes its own result and heater state.

The controller uses outbound Firebase connections for telemetry and commands. Its hotspot serves Wi-Fi setup while disconnected and switches to the pairing code after it joins Wi-Fi. Use **Change Wi-Fi network** from that page if pairing stalls. The hotspot closes once the connected controller is paired.

With fresh telemetry, **Disable heat** remains available during **Readings only**.
The dashboard waits for the controller's confirmation. The request can be
delivered after a reconnect or reboot within its 30-second lifetime.

A new controller appears in the dashboard after it saves its credentials and
confirms pairing. If the controller restarts before saving, use the new code on its setup page.
Claiming a code reserves an identity. The controller's first claimed poll
provisions it, and confirmation activates it. Provisional enrollments expire
24 hours after the code expires. New pairing requests clean up expired
provisional accounts in bounded batches.

Re-enrollment of an existing controller requires its saved recovery key and
the current owner's approval. Older controllers register a key when they
connect with valid Firebase credentials after the firmware upgrade. The setup
page shows **USB enrollment recovery required** if the saved enrollment is
unreadable or recovery after revocation cannot verify the saved key. Reset
enrollment over USB to pair as a new controller.
[USB enrollment reset](../docs/ota-release-guide.md#reset-firebase-enrollment-over-usb)

If setup reports a disabled Firebase account, ask a Firebase administrator to
re-enable the controller's Auth user. The controller retries with its saved
enrollment and prompts for a new recovery code if Firebase rejects the saved token.

## Pair multiple boards

Use the same Firebase test project and an individual setup passphrase and enrollment
for each board. Keep enrolled flash backups with the board that created them.

Each board uses a `P1S-Heat-Setup-<board ID>` hotspot. Match its suffix to the
ID on the setup page. Enter that board's pairing code in the dashboard and use
the same ID to select it in **Controllers**. Boards running older firmware show
their Firebase ID. Pairing another board leaves the current selection in place.

Stagger initial setup for larger batches. The pairing service accepts 12 code
claims per owner in five minutes and 20 code requests per IP in ten minutes.

Verify both boards together with unloaded relay contacts before commissioning
heating. [Two-board bench test](../docs/commissioning-checklist.md#two-board-firebase-pilot)

## Create a dashboard account

1. In the Firebase console, open **Authentication → Sign-in method** and enable **Email/Password**. [Firebase password authentication](https://firebase.google.com/docs/auth/web/password-auth)
2. Open **Authentication → Users → Add user** and enter the owner's email and password. Copy the Firebase UID shown for the account.
3. In the Realtime Database **Data** tab, set `owners/<uid>/enabled` to the boolean `true`. The dashboard requires this approval to view controllers, pair them, or send commands. Set it to `false` to remove dashboard access.

Keep the same Firebase UID when switching an existing controller owner to password sign-in. Set a password on the existing Auth user to preserve the ownership links. [Update a Firebase Auth user](https://firebase.google.com/docs/auth/admin/manage-users#update_a_user)

For projects using Identity Platform, the **User actions** setting can disable self-service signup for the entire Firebase project. [Identity Platform user self-service settings](https://docs.cloud.google.com/identity-platform/docs/concepts-manage-users#user_self-service)

## Run locally

Use Node.js 22.12 or later, JDK 21 or later, and the Firebase CLI on your PATH. On Windows, put the JDK's `bin` directory ahead of the Oracle Java launcher on PATH so the CLI can stop the database emulator between test runs. Run these commands from this directory.

```powershell
npm ci
npm --prefix functions ci
npm run emulators
```

Open the [Emulator Suite](http://127.0.0.1:4000) and add an email and password user in **Authentication**. Approve its UID under `owners/<uid>/enabled` in the `demo-p1s-heat-default-rtdb` database emulator. Sign in at the [local dashboard](http://127.0.0.1:5000).

## Verify

Run the test commands from this directory with the emulator ports available.

```powershell
npm test
npx playwright install chromium
npm run test:browser
```

The browser test signs in with a provisioned password account, pairs a simulated controller, and publishes telemetry using the ESP32's HTTP formats. It verifies desktop and mobile layouts and checks that a command result and the actual relay state update independently. On a machine with Edge installed, set `P1S_BROWSER_CHANNEL` to `msedge` before running the browser test to use that installation.

## Deploy to a Firebase test project

1. Set up a Blaze-enabled Firebase test project with a default Realtime Database and a Web app. Add the first owner as described above. Disable the Google provider in the test project if it was enabled for the previous dashboard. Copy the Web app's public configuration into a local `.env` using `.env.example` as the template.
2. Give the Functions runtime service account access to the test Realtime Database, `firebaseauth.users.create` to provision controllers, `firebaseauth.users.get` to check token revocation, `firebaseauth.users.delete` to remove expired provisional accounts, and permission to sign Firebase custom tokens. [Firebase Authentication IAM permissions](https://firebase.google.com/docs/projects/iam/permissions#authentication) · [Firebase custom token setup](https://firebase.google.com/docs/auth/admin/create-custom-tokens)
3. Use the test project ID below to deploy the database rules, Functions, and dashboard. Firebase prints the permanent Hosting URL when the deploy finishes.

```powershell
$p1sFirebaseProject = '<test-project-id>'
npm run build
firebase deploy --project $p1sFirebaseProject --only database,functions,hosting
```

4. Deploy the updated database rules before flashing firmware that reports a board ID. Build each ESP32 with the same project's database URL and Web API key. Set the firmware's pairing base URL to the permanent Hosting URL followed by `/api/pairing`. Flash directly through the USB-to-UART adapter and follow the bench commissioning sequence with the relay contacts unloaded. [Firmware configuration](../docs/superpowers/plans/2026-11-23-firebase-firmware.md#pilot-build-configuration) · [Direct USB flash](../docs/ota-release-guide.md#one-time-usb-setup) · [Commissioning checklist](../docs/commissioning-checklist.md)

Printer configuration and website-triggered updates depend on the corresponding firmware commands. The website displays the controller's current printer connection and firmware update state. [Website plan](../docs/superpowers/plans/2026-11-23-firebase-website.md#management-actions-after-firmware-support)
