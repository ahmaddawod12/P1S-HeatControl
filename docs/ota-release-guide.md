# OTA setup and releases

Install the Firebase baseline over USB-to-UART. Once the controller is paired
and idle, use **Check and install update** under **Firmware** in the dashboard.
The board verifies each signed update and checks the new firmware at boot.

The shared-relay firmware targets classic ESP32-WROOM-32 controllers with
4 MB flash and an active-high GPIO16 relay that starts LOW (off). Source and signed releases share the public
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

Firmware for the shared GPIO16 relay uses the signed hardware identity
`esp32-wroom-32-1relay`. Controllers with separate GPIO16 and GPIO17 relays
require the shared-relay wiring and unloaded-relay checks before migrating by
USB. OTA accepts updates within the installed hardware profile.

Use the USB-to-UART adapter with the relay loads disconnected. The public
profile in `config/sdkconfig.release` targets the hosted dashboard. When the
setup hotspot starts, copy the `setup passphrase` from the controller's USB
serial log. The controller generates it once and stores it in `device_cfg`.
Use it to join the hotspot and enter the Wi-Fi settings on the setup page.
Keep the passphrase private.

Use an activated ESP-IDF v6.1 PowerShell environment. Select the adapter's COM
port and put the ESP32 in download mode before running the flash command.

```powershell
New-Item -ItemType Directory -Force build-bootstrap | Out-Null
Copy-Item config/sdkconfig.release build-bootstrap/sdkconfig
idf.py -B build-bootstrap -D SDKCONFIG=build-bootstrap/sdkconfig -D P1S_BOOTSTRAP_CREDENTIALS=OFF build
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed.' }
$p1sPort = 'COMx' # Replace with the adapter's port.
Push-Location build-bootstrap
try {
    python -m esptool --chip esp32 --port $p1sPort --baud 115200 --before no-reset --after no-reset write-flash '@flash_args'
    if ($LASTEXITCODE -ne 0) { throw 'Firmware flash failed.' }
} finally {
    Pop-Location
}
```

Reset the board into normal boot after the write completes. This command uses
the build's flash offsets and writes directly, without a flash backup. It
resets the OTA boot selection and keeps existing Wi-Fi and pairing data when
the board uses the same partition layout. If the write is interrupted, enter
download mode and flash the image again. Published OTA packages use the
signed release process below.

On first boot, the image stays in maintenance until the runtime and sensor
tasks produce fresh samples, the outputs are safe, storage is healthy, and
either Wi-Fi and pairing are complete or the setup portal is ready. A known
physical sensor fault can remain safely latched. The sensor task must be active. The deadline is two
minutes. With no previous confirmed image, failed setup stays inhibited for
diagnosis and can be recovered over USB. Verify the hosted dashboard before
enabling any load. Normal confirmed firmware keeps regulating during later
router outages.

ESP-IDF initializes the first empty OTA slot as valid itself. Each Firebase
controller records completion of local health checks in `device_cfg` using the
firmware image hash. The check applies to pilots provisioned through the setup
portal as well as those with compiled Wi-Fi credentials. Later boots of a
confirmed image can leave maintenance without needing the router.

## Reset Firebase enrollment over USB

Use this procedure when the setup page shows **USB enrollment recovery
required**. It erases the controller's `device_cfg` partition, including saved
Firebase enrollment, Wi-Fi and printer settings, and boot and update records.
An interrupted heat session still requires acknowledgment once the new dashboard
connection is available. The next pairing creates a new Firebase device identity.

Disconnect the relay loads, close any serial monitor, and put the ESP32 in
download mode. From the ESP-IDF v6.1 PowerShell environment, select the
USB-to-UART adapter's COM port and erase the named partition.

```powershell
$p1sPort = 'COMx' # Replace with the adapter's port.
$p1sPartTool = Join-Path $env:IDF_PATH 'components/partition_table/parttool.py'
python $p1sPartTool --esptool-args before=no-reset after=no-reset --port $p1sPort --baud 115200 erase_partition --partition-name device_cfg
if ($LASTEXITCODE -ne 0) { throw 'Device configuration erase failed.' }
```

Reset the controller into normal boot and follow the
[Wi-Fi setup and pairing instructions](../README.md#wi-fi-setup-and-dashboard-access).

## Release through GitHub Actions

Use **Actions → Release firmware → Run workflow** to release the version on
`main`. Enable **Build and sign without publishing a release** to produce a
signed artifact without changing Git tags or GitHub releases.

Before the first cloud release, add a repository Actions secret named
`OTA_SIGNING_PRIVATE_KEY` in **Settings → Secrets and variables → Actions**.
Its value is the existing ECDSA private key's complete PEM text, including the
BEGIN/END lines. It must match `config/ota_signing_public.bin`; generating a new
key would break trust with installed controllers. GitHub documents
[secret setup](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets)
and [manual workflow runs](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow).
No separate publication token is needed: the publishing job uses `GITHUB_TOKEN`.

For example, to release `1.0.1`:

1. Set `version.txt` to `1.0.1` and write `docs/releases/v1.0.1.md` with the
   changes and any commissioning limitations.
2. Commit those files with the intended firmware changes and merge into `main`.
3. Run **Actions → Release firmware**. The workflow runs the Windows host,
   setup page and local-publisher tests, builds the generic ESP-IDF 6.1 firmware
   and Unity test image, runs the Python tool tests and compiled stack check,
   signs and validates the package, and publishes `v1.0.1` at the tested commit.
4. The final job checks GitHub's latest stable release and anonymously downloads
   and validates both public assets. The dashboard can then install the release
   through **Check and install update**.

The workflow reads `version.txt` for the compiled image and the release tag.
New versions must be stable `major.minor.patch`,
newer than every existing stable tag, and have nonempty committed release notes.
The checkout must match the workflow commit and remain clean. Only upstream
`main` can run the release jobs; forks and other branches cannot publish.

An existing version tag makes the run a successful no-op. The workflow never
replaces a tag or release. If a build or signing check fails, fix the problem and
run the workflow on `main` again. If publication fails after reserving a tag,
inspect that tag/release before recovery; a retry will not replace it. A failed
public-download check can occur after publication, so inspect the existing
release before deciding to issue another version.

The signing job writes the secret only to a temporary runner file, mounts it
read-only during signing, and removes it afterward. Its artifact and the release
contain exactly the signed application and `firmware.json`. Only the final job
has repository write permission. Personal configurations, USB setup images and
backups are not used by this workflow. Release runs on `main` are serialized;
a manual run on another branch cannot displace them. GitHub keeps one running
and the newest pending run per branch, so superseded pending versions are not
published when several version bumps arrive during a build.

## Prepare a generic public release locally

The local process remains available when signing from the owner's PC is preferred.

Increase `version.txt` using stable `major.minor.patch` versions. Keep the board,
layout, relay polarity and trust key compatible. Use separate build/config
paths so personal credentials cannot contaminate the release configuration.
Use the committed public profile and a fresh build directory. Public images
generate their setup passphrases on the device.
Commit and push the intended source revision before the final build and signing.

```powershell
$p1sVersion = (Get-Content version.txt -Raw).Trim()
New-Item -ItemType Directory -Force build-release | Out-Null
Copy-Item config/sdkconfig.release build-release/sdkconfig
idf.py -B build-release -D SDKCONFIG=build-release/sdkconfig -D P1S_BOOTSTRAP_CREDENTIALS=OFF build
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

Stop **Monitor P1S** before updating; update requests are rejected while it is
active so preflight cannot interrupt monitored heating. Stopping monitoring
keeps manual heating enabled. Press **Update firmware** once. A local worker checks the newest stable release
without changing heating during an unsuccessful preflight. An equal/older
release reports up to date. A newer compatible release starts maintenance,
switches off the heater and fan, then streams to the inactive slot.
Target changes, enable and fault acknowledgement are blocked
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
