# Wi-Fi configuration and fallback hotspot

Implemented for version 1.0.5 in the existing checkout. The dashboard shows the
stored SSID and password as editable plain text, as requested. Save persists the
exact strings and connects without restarting. Missing credentials start the
open `P1S-Heat-Setup` hotspot immediately; an unreachable configured network
starts it after 30 seconds. Both interfaces serve the same dashboard. Successful
station connection closes the hotspot after 10 seconds; failed credentials
leave it available at `http://192.168.4.1/`.

No partition, signing key, Wi-Fi record or settings schema changed. A shared
operation gate prevents Wi-Fi writes during OTA preflight or a real pending
trial. Initial setup without a previous rollback image remains configurable
after its health deadline. Network changes do not enable outputs or clear
latched faults.

## Verification

- Host: 17/17 suites passed, including real atomic admission races, replacement
  persistence and failure handling, exact fallback/grace timing and strict JSON.
- Browser: 18/18 tests passed across firmware and Wi-Fi behavior, including
  visible fields, preserved drafts, uncertain replies and reconnect guidance.
- Packaging: 10/10 tests passed; all four fake-publisher repository cases passed.
- Browser preview with dummy credentials: visible password, matching styles,
  Save and the station-address/grace instructions inspected.
- ESP-IDF generic, personal bootstrap and native test builds passed. Signed
  generic/personal validation and public publication dry run passed; the generic
  image was also checked for absence of the local personal credentials.
- On-board Unity: 93/93 passed with no failures, errors or skipped cases.
  The first run's sole failure was an older asset test that prohibited local
  controller links; it now checks loaded resources. Three existing pytest
  harness warnings concern its mark and XML-report API/format.
- A fresh review found three recovery races. Regression tests failed before
  fixes and passed afterward: superseded browser saves, discovery after an
  unseen address change, and AP shutdown racing a disconnect or operation.
- Missing-credentials startup on the actual board: generic 1.0.5 enabled the
  setup AP at 925 ms and started HTTP at 955 ms, with no startup panic during
  the 12-second probe. Wi-Fi-client access was not exercised in this probe.
- COM3 board's 61,440-byte NVS/config/OTA metadata backup saved privately before
  native test firmware. Baseline 1.0.4 has both outputs off, target 60 °C,
  calibration 0 °C and its sensor-short fault latched.

After the startup probe, the confirmed signed 1.0.4 application and all 61,440
bytes of original NVS/configuration/OTA metadata were restored with flash hash
verification. HTTP readback confirms 1.0.4 is locally confirmed, joined the
original Wi-Fi at 10.0.0.31, target is 60 °C, all outputs are off, and the
sensor-short fault remains latched. No eFuse settings were changed.

## Published update and manual handover

[Public 1.0.5](https://github.com/ahmaddawod12/P1S-HeatControl-Releases/releases/tag/v1.0.5)
was explicitly approved and published with only the signed generic binary and
manifest. Both assets were downloaded anonymously and their signature/digest
verified. The binary SHA256 is
`420874203209c504336cc7ee681e08fbad7efb425664ede134057122ac681ab1`.

Actual GitHub OTA from confirmed 1.0.4 to 1.0.5 completed and locally confirmed
in 38.61 seconds. It received/verified 1,179,636 bytes, retained saved Wi-Fi and
target, and kept outputs off. Saving the current network details on 1.0.5 also
passed: the station remained connected, AP opened then closed at 10.66 seconds,
and the boot ID did not change.

The automatic wrong-password client test did not reach the AP web endpoint.
Windows recorded a canceled AP association after two seconds and returned to its
original network. A later RF scan positively identified `P1S-Heat-Setup` as an
open network on channel 11. This is evidence of broadcast, not a passed client
recovery test. The confirmed 1.0.5 NVS/config/OTA metadata backup was restored
with flash hash verification, and the temporary PC test profile was removed.

The owner chose to perform the remaining tests manually. Automatic Wi-Fi changes
have stopped. Last readback: confirmed 1.0.5, connected to the original network at
10.0.0.31, Save available, AP off, target 60 °C, both outputs off, sensor-short
fault latched. The owner subsequently reported that manual Wi-Fi testing and
password correction work. When they asked about the hotspot remaining visible,
live readback showed connected=true, applying=false, can_save=true and AP=false,
with no OTA maintenance or pending trial. The intended grace period is ten
seconds after station connection succeeds. The actual 30-second unavailable-
network startup timing remains unmeasured on hardware.

The owner's original staged migration is unchanged. Main Wi-Fi/server
integration and existing documentation/test paths that were already staged
remain as unstaged working-tree changes; retain the complete checkout for
continued work. New and otherwise owned files are committed separately.
