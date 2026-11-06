# OTA setup and releases

The dashboard updates classic ESP32-WROOM-32 controllers with 4 MB flash and
active-high GPIO16/GPIO17 relays: OFF is LOW, ON is HIGH, and both relays start
OFF. Source and signed releases share the public
[`ahmaddawod12/P1S-HeatControl`](https://github.com/ahmaddawod12/P1S-HeatControl)
repository. Version **1.0.0** starts this release series.

Earlier development firmware, including 1.0.5, points to the former
`P1S-HeatControl-Releases` repository and refuses an older version. Install this
1.0.0 baseline over USB once to adopt the new repository and relay polarity.
Keep the normal OTA version checks enabled. Future updates must increase the
version and retain the selected hardware profile.

Earlier commissioning results in the [implementation status](ota-implementation-status.md)
are historical evidence for the previous builds. Physical relay and OTA
commissioning of this reset baseline remains a separate check.

## One-time USB setup

The previous single-image flash layout cannot install the first OTA image from
the dashboard. Use USB once, with both relay loads disconnected. Keep a copy of
the existing commissioning record, target and calibration. The installer backs
up the existing settings partition and preserves it; it erases only the new
device configuration/OTA metadata area before installing the new layout.

Use an activated ESP-IDF **v6.1** PowerShell environment. `python` must be its
Python environment, with `espsecure` and `esptool` available. Create the ignored
`sdkconfig.secret` as described in the README. The private ECDSA signing key
must stay outside this checkout; back it up securely. Losing
that key prevents signing updates trusted by the installed firmware. Only the
raw public key, `config/ota_signing_public.bin`, belongs in Git.

```powershell
idf.py -B build-bootstrap -D SDKCONFIG=sdkconfig.bootstrap -D P1S_BOOTSTRAP_CREDENTIALS=ON build
$p1sKey = 'C:/path/outside-checkout/ota_signing_private.pem'
./tools/install_bootstrap.ps1 -BuildDir build-bootstrap -PrivateKey $p1sKey
```

That command signs and verifies only. When the physical checks are ready, select
the actual port explicitly and run:

```powershell
./tools/install_bootstrap.ps1 -BuildDir build-bootstrap -PrivateKey $p1sKey -Port COMx -Flash
```

The script checks the chip, offsets, fixed partition layout, signature, hardware
profile and personal build flag before writing. The settings backup is under
`build-bootstrap/private-backups/`. Keep the entire personal build and backups
private: they contain the Wi-Fi password. Do not use the generated unsigned
`idf.py flash` command for this setup. No eFuse or hardware secure-boot changes
are required.

On first boot, the image stays in maintenance until fresh runtime and sensor
tasks, safe outputs, usable saved settings/credentials, a Wi-Fi IP and the local
HTTP service are healthy. A known physical sensor fault can remain safely
latched; an absent/stalled sensor task cannot pass. The deadline is two minutes.
With no previous confirmed image, failed setup stays inhibited for diagnosis;
USB recovery remains available. Verify the local dashboard before enabling any
load. Normal confirmed firmware keeps regulating during later router outages.

ESP-IDF initializes the first empty OTA slot as valid itself. A separate
committed `boot_ok_v1` record in `device_cfg` therefore records completion of
the personal bootstrap's local health checks. Until that record exists, the
bootstrap stays in maintenance regardless of the SDK slot state. Later boots
with that record can leave maintenance without needing the router.

## Prepare a generic public release

Increase `version.txt` using stable `major.minor.patch` versions. Keep the board,
layout, relay polarity and trust key compatible. Use separate build/config
paths so personal credentials cannot contaminate the release configuration.
When reusing older build directories, select active-high relays and set
`CONFIG_P1S_RELEASE_REPOSITORY="ahmaddawod12/P1S-HeatControl"` in their
sdkconfig files; defaults do not replace saved selections.
Commit and push the intended source revision before the final build and signing.

```powershell
$p1sVersion = (Get-Content version.txt -Raw).Trim()
idf.py -B build-release -D SDKCONFIG=sdkconfig.release -D P1S_BOOTSTRAP_CREDENTIALS=OFF build
python -m espsecure sign-data --version 1 --keyfile $p1sKey --output build-release/p1s-heat-controller-esp32-4mb.bin build-release/p1s_heat_controller.bin
python tools/package_firmware.py --build-dir build-release --public-key config/ota_signing_public.bin --version $p1sVersion --output "dist/firmware-$p1sVersion"
./tools/publish_firmware.ps1 -PackageDir "dist/firmware-$p1sVersion" -Version $p1sVersion
```

The publisher defaults to a local dry run. Review the concrete package and
`docs/releases/v<version>.md` when present; otherwise it uses generic release
notes. Commit the source used for the build and push that commit to this
repository before adding `-Publish`:

```powershell
git push origin HEAD
./tools/publish_firmware.ps1 -PackageDir "dist/firmware-$p1sVersion" -Version $p1sVersion -Publish
```

Publication requires a clean source tree and an existing public repository with
source history. The release tag targets the exact local HEAD commit. The tool
does not create a repository, initialize history or change visibility. It
refuses any existing version tag, rebuilds the committed source using ESP-IDF,
and checks that the signed package payload matches that application. Use
`-BuildDir` if the generic build directory is not `build-release`. A stale
package must be signed and packaged again from the rebuilt output. It uploads
exactly:

- `p1s-heat-controller-esp32-4mb.bin`: signed application only, at most `0x1F0000` bytes.
- `firmware.json`: format, product, hardware, layout, version, filename, full
  signed-file size and SHA-256 digest.

Personal bootstraps, SDK configurations, keys and flash backups stay local.
GitHub also provides source archives for the tagged public commit. Existing
release tags are not overwritten. Publish normal stable
releases: drafts and prereleases do not become the device's latest release.
The package validator checks the real ECDSA signature, signed identity, ESP32
header/application descriptor, version, partition table and generic build
configuration. Signed descriptor format 2 also records bootstrap purpose and
compiled credential presence; public packaging and device OTA reject either
flag. Packaging matches the signed payload to that build's application output,
and publication rechecks the signed purpose independently of build metadata.
Both generic and personal validation require the current update repository and
active-high profile, so an older saved sdkconfig cannot silently select them.
Changing the release repository requires updating the fixed device
configuration before installing the bootstrap.

After publication, verify that GitHub's latest-release metadata and both asset
download URLs work anonymously. No GitHub token belongs on the controller.
Source and compiled generic firmware are public. This signing scheme
authenticates OTA images; hardware secure boot is
not enabled.

## Dashboard behavior and recovery

Press **Update firmware** once. A local worker checks the newest stable release
without changing heating during an unsuccessful preflight. An equal/older
release reports up to date. A newer compatible release starts maintenance,
disables heating/manual fan, observes the normal fan cooldown, then streams to
the inactive slot. Target changes, enable and fault acknowledgement are blocked
during maintenance. Digest, identity and native signature verification must all
pass before the boot slot changes.

The dashboard polls local progress and survives the reboot. It never retries an
uncertain POST automatically. The new version is successful only after local
boot confirmation; failed trial health returns to a previous confirmed image.
Heating remains disabled after either boot. Wi-Fi credentials, target and
calibration survive normal OTA updates. A failed or interrupted update is not
retried automatically. A failed safe-off acknowledgement keeps maintenance
latched and requires diagnosis.

Keep USB accessible until the unloaded-relay tests in the commissioning
checklist pass. The earlier `1.0.0 → 1.0.3 → 1.0.4` sequence used the former
repository and hardware profile. Commission the new baseline separately and exercise
interrupted downloads/power, bad signatures, startup stalls, rollback, saved
settings and dashboard recovery. A build or host test does not demonstrate
these physical results. See the [historical implementation and test status](ota-implementation-status.md)
for earlier on-board checks, pending OTA tests and the private settings backup.
