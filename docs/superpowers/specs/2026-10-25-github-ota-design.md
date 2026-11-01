# GitHub firmware update design

Date: 2026-10-25

Status: Approved by the owner on 2026-10-25. The owner selected GitHub downloads and public firmware releases with the source repository remaining private. A builder review found no further questions before planning.

## Intent and agreed behavior

The controller will be installed inside a printer where removing the board for routine firmware updates is impractical. The owner wants to open the existing local dashboard, press **Update firmware**, and have the ESP32 download and install the new release itself.

The owner accepted GitHub Releases as the download source, with progress reporting, heating disabled during installation, first-boot checks and rollback, and Wi-Fi credentials preserved separately from the firmware. The owner also selected a separate public firmware-release repository while keeping the existing source repository private. One initial USB installation is required to introduce OTA support and the new partition table. These are application updates; the dashboard assets are embedded in the same application image and update together.

The detailed choices below are proposals by the author, rather than additional requirements supplied by the owner.

## Current project constraints

- Classic ESP32-WROOM-32, ESP-IDF 6.1, 4 MB flash, UART0 console.
- `partitions.csv` currently defines NVS, PHY data, and one factory application. Rollback is disabled in the current configuration.
- The existing application binary is 917,488 bytes, built on 2026-10-11. This is a sizing reference, not a fresh build of the staged source.
- Wi-Fi credentials currently come from the local, ignored `sdkconfig.secret`; the Wi-Fi driver uses RAM storage. Generic release firmware must instead load credentials from device storage.
- The runtime task owns the relay outputs. Commands arrive through a queue. A successful enqueue does not prove the heater is off, and `app_runtime_stop()` does not explicitly switch the relays off.
- Heating always starts disabled. Disable normally gives the fan a 10-second cooldown. Manual fan operation is an independent override.
- The dashboard is served over local HTTP and has no application password. The new updater must not accept caller-selected firmware URLs or image uploads.
- The source repository is `ahmaddawod12/P1S-HeatControl`. A read-only GitHub CLI check on 2026-10-25 reports that it is private and has no releases.
- Existing staged changes in the checkout are unrelated working material and must be preserved.

## Goals, exclusions, and failure criteria

Goals are one-button installation of a published compatible release; continued access to the dashboard after a normal update; preserved Wi-Fi, target, and calibration; and recovery to the previous confirmed application after a failed trial boot. Normal temperature regulation must continue to work without internet access.

Version 1 excludes scheduled or unattended installation, automatic installation after a power cycle, prereleases, arbitrary update servers selected through the API, browser file uploads, runtime Wi-Fi provisioning, bootloader or partition-table updates over the network, and changing hardware secure-boot eFuses. It does not add a general dashboard account system.

Unacceptable outcomes include heating during a flash operation, accepting enable commands while maintenance is active, replacing the current application before the new image is fully validated, losing credentials during an ordinary upgrade or rollback, publishing a device's credentials, or reporting success merely because a download completed.

## Approaches

| Approach | Tradeoff | Decision |
| --- | --- | --- |
| Device downloads a published GitHub release | Fits the requested button; requires release assets and a separate public distribution repository | Selected by the owner |
| Device downloads from a dedicated HTTPS host | Same device behavior, but requires another host and release publication process | Not selected |
| Browser uploads a locally downloaded binary | Avoids device access to a release service, but requires the owner to obtain the file for every update | Does not meet the requested workflow |

The owner selected the first of these distribution approaches:

1. **Public firmware assets in a separate release repository, with source remaining private.** Selected. The controller requires no GitHub token. Anyone can download and inspect a released binary. The proposed repository name is `ahmaddawod12/P1S-HeatControl-Releases`; its availability must be checked before creation. It contains release files and installation notes, without copying private source or device secrets.
2. **Private release assets with a restricted device token.** Considered but not selected. This adds token provisioning, storage, expiry, and revocation to an otherwise public-firmware update path.

Implement anonymous HTTPS downloads from the separate public release repository. The existing source repository stays private. Actual repository creation and release publication belong to implementation and release work after design review; neither has been performed during this design task.

## Flash layout and initial installation

The proposed fixed 4 MB layout preserves the existing settings partition location and reserves a separate NVS partition for device configuration:

| Name | Type/subtype | Offset | Size |
| --- | --- | --- | --- |
| `nvs` | data/nvs | `0x9000` | `0x6000` |
| `phy_init` | data/phy | `0xF000` | `0x1000` |
| `device_cfg` | data/nvs | `0x10000` | `0x6000` |
| `otadata` | data/ota | `0x16000` | `0x2000` |
| `ota_0` | app/ota_0 | `0x20000` | `0x1F0000` |
| `ota_1` | app/ota_1 | `0x210000` | `0x1F0000` |

The partition table remains at `0x8000`, subject to verifying that the new bootloader fits below it. Each application slot is 2,031,616 bytes. There is no separate factory application. Reject any signed application larger than a slot, and make slot-size and bootloader-size checks release gates.

The initial USB installation writes the OTA-capable bootloader, partition table, initial signed application in `ota_0`, and initialized OTA selection state. Because the old factory image overlaps the new device configuration area, follow a documented migration procedure rather than treating this as an application-only flash. Preserve settings when compatible; seed device credentials only after the new layout is installed. Confirm the initial application before allowing its first OTA operation.

The board should pass a real update and rollback rehearsal while USB remains accessible. OTA reduces routine physical access; it cannot guarantee recovery from arbitrary firmware, flash, or hardware failure.

## Persistent device configuration

Add a small validated, versioned store in `device_cfg`, initialized by label and accessed through explicit APIs. It stores Wi-Fi credentials and small update-attempt/outcome records. No GitHub token is needed or stored. Keep target and calibration in the existing `nvs` settings store.

During the local initial setup build, valid compiled-in Wi-Fi credentials seed an absent device configuration once. Existing stored credentials always take precedence. Later release builds contain empty Wi-Fi defaults and continue loading the stored values. Never distribute this personal setup build as a release artifact.

Do not erase `device_cfg` automatically on initialization errors, missing keys, failed validation, or a firmware schema change. Report the failure and keep heating disabled if safe initialization is impossible. Device configuration migrations must be additive and readable by both the candidate and the rollback image; do not remove old keys during a trial boot. Application settings migrations must likewise retain rollback compatibility.

Version 1 does not claim that ordinary NVS storage protects secrets against physical flash readout. Storage encryption and changing Wi-Fi networks from the dashboard are separate work.

## Release contract and trust

Use explicit stable semantic versions, for example `v1.2.3`. A release contains a signed application named `p1s-heat-controller-esp32-4mb.bin` and a bounded `firmware.json` manifest. The manifest identifies its format version, product, hardware variant, OTA layout compatibility, firmware version, application filename, signed file size, and SHA-256 digest. The signed application must embed the same product/hardware/layout identity; untrusted metadata alone cannot authorize a different board image.

Query GitHub's latest full release endpoint for the selected repository. Reject drafts, prereleases, malformed versions, missing or duplicate required assets, unsupported manifest formats, oversized responses, and incompatible images. Skip versions equal to or older than the installed release. A previous failed version is reported with its recorded reason; it is never retried automatically, although another deliberate button press may retry it.

Use HTTPS with hostname and certificate-chain verification through the ESP-IDF certificate bundle. Synchronize the clock before a release check when needed, using a bounded wait; failure must not cause certificate verification to be bypassed. Download only assets associated with the selected release. Follow bounded HTTPS redirects needed for GitHub asset delivery, without an Authorization header. Treat redirected asset URLs as sensitive and do not log their query strings.

Use ESP-IDF signed-app verification on OTA updates without enabling hardware secure boot. The proposal is its ECDSA version-1 signature format, compatible with the project's minimum ESP32 revision 0 configuration. Embed only the verification key. Keep the private signing key outside the repository and device, with a backed-up owner-controlled copy; release signing uses a protected CI secret or an owner-controlled local signing step. Do not enable eFuse anti-rollback or disable serial recovery.

The release build must use ESP-IDF 6.1, the agreed board configuration, the OTA layout, rollback enabled, generic device configuration defaults, and the trusted verification key. Validate the signed output, compatibility metadata, and digest before publication. Publishing a version is an explicit release action, not a side effect of every push. Initial publication can use a documented local build/sign/upload process; CI automation is optional and must not block implementing the device update path.

## Runtime interlock and update worker

Add an OTA service running independently of the HTTP server task and below the temperature/control task priorities. Its network operations, buffers, deadlines, and error results are bounded. Stream firmware into the inactive slot instead of buffering an application in RAM. Keep the web server and temperature/runtime tasks alive throughout the operation.

The worker proceeds as follows:

1. **Checking:** verify network/time and fetch release metadata. Heat regulation continues during this read-only preflight. An unavailable server, missing release, GitHub rate limit, or unchanged version causes no reboot and no unnecessary heating interruption.
2. **Cooling:** after finding a newer compatible candidate, request exclusive maintenance mode from the runtime. This is a runtime interlock, not just disabled dashboard controls. Atomically prevent concurrent enable requests, discard or reject earlier queued enable/manual-fan-on commands, command heating disabled, and acknowledge only after the runtime has applied heater-off successfully. Complete the 10-second disable cooldown, then clear any manual fan request and require both reported outputs off. Abort without writing firmware if the maintenance acknowledgement or safe output state cannot be obtained within a bounded deadline.
3. **Downloading:** acquire the inactive OTA slot and stream the release asset. Keep maintenance latched. Reject enable, target-change, manual-fan-on, and fault-acknowledgement requests with `409` during maintenance; Disable remains idempotent and accepted. The lock must cover direct runtime callers, not merely REST handlers. Keep sensor monitoring and fault handling active. Flash writes must never race a controller request to energize the heater.
4. **Verifying:** check complete byte count, file digest, image signature, signed compatibility identity, and ESP-IDF image validation. Do not change the next boot selection until all checks pass. On failure, abort the OTA session, retain the confirmed application, report the cause, and leave heating disabled. Release the maintenance gate only after cleanup and safe output confirmation; a subsequent explicit Enable is required.
5. **Rebooting:** record the candidate version and previous confirmed version, select the new boot slot, expose the rebooting state, and restart. Power loss while writing the inactive application must leave the previously selected application available.

Use only one worker/session at a time. Concurrent update requests return `409`. The browser supplies neither a firmware URL nor release credentials. A failed preflight can be retried with another click; an in-progress transfer is not automatically resumed in version 1.

Output-off acknowledgement is software confirmation through the existing board driver. The current hardware has no relay contact or current feedback, so this design does not claim physical verification that a relay opened.

## Trial boot and rollback

Enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A newly installed candidate boots with heating and manual fan disabled, and maintenance remains active until its trial completes. Start a proposed 120-second trial deadline early in startup, independently of the existing service-retry loop.

Confirmation requires: board-output initialization succeeded and remains inactive; settings and device configuration loaded without destructive migration; sensor and runtime tasks are producing fresh status; station Wi-Fi regained an address; and the local HTTP server can successfully service a status request. A correctly reported physical sensor fault may remain latched and visible, provided sampling and safe fault handling are operating. Do not require internet, GitHub reachability, mDNS discovery, or an open browser to confirm the application.

Use a watchdog/health supervisor capable of resetting a stalled trial; a timer callback alone is insufficient if startup or the health task hangs. Run health checks within the finite trial window. On success call `esp_ota_mark_app_valid_cancel_rollback()`, record success, release maintenance, and continue with heating disabled. On explicit failure or deadline expiry mark the candidate invalid and reboot to the previous confirmed image. An unexpected reset while pending also permits bootloader rollback.

Retain the existing settings and credentials on rollback. Compare the attempt record with the running confirmed version to report an interrupted or rolled-back attempt, including when power loss prevented recording an error. Do not automatically install the same release again. Once a version is confirmed, ordinary router outages must retain the existing offline-regulation behavior rather than trigger rollback.

## Dashboard and API behavior

Extend the existing dashboard with installed version, **Update firmware**, and a compact status/progress display. One click checks and installs a newer compatible stable version; there is no file picker, URL entry, or second confirmation dialog. Disable update and heating controls while maintenance is active, and show explicit unchanged-version, connection, verification, failure, reboot, and rollback results.

Proposed API:

- `GET /api/firmware`: installed version, update phase, candidate version when known, bytes received/total, and a bounded result/error code. It performs no external update check.
- `POST /api/firmware/update`: request the single check-and-install worker; return `202` on acceptance, `409` when busy or a trial is pending, and existing validation/service errors as appropriate.

Keep this snapshot separate from the existing fixed-size status/SSE buffers. The builder can choose a separate SSE firmware event or lightweight polling. The dashboard reconnects to the same device address after restart and reloads its embedded assets after observing a changed running version. Serve update state and dashboard assets with cache behavior that prevents reporting stale success.

Protect the new mutation endpoint against browser cross-site requests using a per-boot token in a custom header, expected content type, and Host/Origin checks supporting the device's IP and mDNS address. Do not enable cross-origin access. This is CSRF protection, not authentication: under the existing LAN trust model another local client can deliberately request installation of an official signed release. A broader password/account system is outside this design. Device configuration secrets are never returned through the local API.

## Verification and commissioning

Implementation validation must cover the consequential boundaries rather than only mirror helper functions:

- Release/version parsing, compatibility checks, oversize/truncated responses, absent assets, and malformed manifests.
- Credential seeding only when absent, survival across generic firmware replacement, no secret defaults in release artifacts, and rollback-compatible settings/configuration access without automatic device-config erasure.
- Real runtime ordering: accepted update versus queued/concurrent Enable or manual fan commands, cooldown completion, output acknowledgement failure, cleanup, and duplicate update requests.
- Download interruption, timeout, API rate limits and server errors, bounded HTTPS redirects, incorrect digest, invalid signature, incompatible signed image, insufficient flash/heap, and unchanged boot selection on failed verification.
- Host tests for pure policy and parsing, on-target component tests for orchestration/storage/API boundaries, a complete application build, and explicit slot/bootloader sizing checks.
- On the real ESP32 with relay contacts unloaded: successful update in both slot directions, browser reconnection, no automatic heating after reboot, settings preservation, power interruption during transfer, rejected trial boot, stalled trial reset, and rollback retaining Wi-Fi/dashboard access. Also verify that an ordinary Wi-Fi outage after confirmation still leaves regulation operating normally.

Do not claim hardware recovery has passed from mocks or a successful compilation alone. Update the README and commissioning checklist with initial USB setup, generic release creation/signing, actual release access, and the required hardware rehearsal before installing the board where USB is inaccessible.

## Owner decisions and builder discretion

Owner decisions: one-button downloads from GitHub Releases; public compiled firmware in a separate release repository; private source remaining in the existing repository. These decisions do not require changing the source repository's visibility or placing a token on the controller.

Author proposals for review: stable releases only; two application slots without a third factory image; separate device-configuration NVS; ECDSA update signatures without eFuse changes; a 120-second trial requiring restored local Wi-Fi and HTTP service; a local-LAN trust model with CSRF protection; and manual release publication initially.

The builder may settle internal C interfaces, task stack sizes and bounded retry/deadline values, metadata encoding inside the signed image, JSON response buffer sizes, and progress transport, provided the invariants and tests above hold. If bootloader size requires a different partition-table offset, revise the layout before the initial USB migration rather than silently moving it in a later OTA release.

## Primary references

- [ESP-IDF 6.1 OTA APIs and rollback](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/ota.html)
- [ESP-IDF 6.1 HTTPS OTA](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/esp_https_ota.html)
- [ESP-IDF signed OTA without hardware secure boot](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/security/secure-boot-v1.html#secure-ota-updates-without-secure-boot)
- [GitHub Releases API and release access permissions](https://docs.github.com/en/rest/releases/releases#get-the-latest-release)
