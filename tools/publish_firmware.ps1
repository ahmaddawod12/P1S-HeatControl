param(
    [Parameter(Mandatory=$true)][string]$PackageDir,
    [Parameter(Mandatory=$true)][string]$Version,
    [string]$Repository = 'ahmaddawod12/P1S-HeatControl',
    [string]$Python = 'python',
    [string]$BuildDir = 'build-release',
    [switch]$Publish
)
$ErrorActionPreference = 'Stop'
$p1sRoot = Split-Path -Parent $PSScriptRoot
$p1sPackage = (Resolve-Path -LiteralPath $PackageDir).Path
if ($Repository -notmatch '^[A-Za-z0-9-]+/[A-Za-z0-9_.-]+$') {
    throw 'Select a GitHub owner/repository.'
}
& $Python (Join-Path $PSScriptRoot 'package_firmware.py') --verify-package $p1sPackage --public-key (Join-Path $p1sRoot 'config/ota_signing_public.bin') --version $Version | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Public package validation failed.' }
$p1sBinary = Join-Path $p1sPackage 'p1s-heat-controller-esp32-4mb.bin'
$p1sManifest = Join-Path $p1sPackage 'firmware.json'
Write-Output "Verified public release: $Repository, tag v$Version"
Get-Item -LiteralPath $p1sBinary,$p1sManifest | Select-Object Name,Length
if (-not $Publish) {
    Write-Output 'No repository or release was changed. Add -Publish to publish these two files.'
    return
}
function Get-P1sSourceCommit {
    $p1sStatus = & git -C $p1sRoot status --porcelain=v1 --untracked-files=normal
    if ($LASTEXITCODE -ne 0 -or $p1sStatus) { throw 'Commit the firmware source changes before publishing.' }
    $p1sSourceCommit = & git -C $p1sRoot rev-parse --verify HEAD
    if ($LASTEXITCODE -ne 0 -or $p1sSourceCommit -notmatch '^[0-9a-f]{40}$') { throw 'Could not identify the firmware source commit.' }
    return $p1sSourceCommit
}
$p1sCommit = Get-P1sSourceCommit
$p1sRepositoryInfo = & gh repo view $Repository --json visibility,defaultBranchRef 2>$null
if ($LASTEXITCODE -ne 0) {
    throw 'The release repository must already exist and be accessible.'
}
$p1sRepositoryState = $p1sRepositoryInfo | ConvertFrom-Json
if ($p1sRepositoryState.visibility -ne 'PUBLIC') { throw 'The selected release repository must already be public; visibility was not changed.' }
if (-not $p1sRepositoryState.defaultBranchRef) {
    throw 'Push the source history to the release repository before publishing.'
}
$p1sExistingTag = & git ls-remote --tags "https://github.com/$Repository.git" "refs/tags/v$Version"
if ($LASTEXITCODE -ne 0) { throw 'Could not verify the remote release tag; nothing was published.' }
if ($p1sExistingTag) { throw 'The release tag already exists; existing tags are not reused or overwritten.' }
if (-not $env:IDF_PATH) { throw 'Publish from an activated ESP-IDF environment so the source can be rebuilt.' }
$p1sIdf = Join-Path $env:IDF_PATH 'tools/idf.py'
if (-not (Test-Path -LiteralPath $p1sIdf -PathType Leaf)) { throw 'ESP-IDF build tool was not found.' }
$p1sBuild = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $p1sRoot $BuildDir }
& $Python $p1sIdf -C $p1sRoot -B $p1sBuild -D P1S_BOOTSTRAP_CREDENTIALS=OFF reconfigure build
if ($LASTEXITCODE -ne 0) { throw 'Source rebuild failed; nothing was published.' }
& $Python (Join-Path $PSScriptRoot 'package_firmware.py') --verify-package $p1sPackage --build-dir $p1sBuild --public-key (Join-Path $p1sRoot 'config/ota_signing_public.bin') --version $Version | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Package does not match the rebuilt source. Sign and package that build before publishing.' }
if ((Get-P1sSourceCommit) -cne $p1sCommit) { throw 'Source changed during verification; nothing was published.' }
$p1sNotesPath = Join-Path ([IO.Path]::GetTempPath()) ('p1s-release-' + [Guid]::NewGuid().ToString('N') + '.md')
try {
    $p1sReleaseNotes = Join-Path $p1sRoot "docs/releases/v$Version.md"
    if (Test-Path -LiteralPath $p1sReleaseNotes -PathType Leaf) {
        Copy-Item -LiteralPath $p1sReleaseNotes -Destination $p1sNotesPath
    } else {
        @"
Signed firmware $Version for the classic ESP32-WROOM-32 P1S controller with 4 MB flash and active-high relays (OFF=LOW, ON=HIGH). Both relays start OFF.

Use the dashboard's Update firmware button after the initial USB OTA bootstrap. Heating stays disabled after installation. firmware.json describes the signed application and its SHA-256 digest.

The two firmware assets contain no personal Wi-Fi credentials. Saved credentials are retained on each device. Source is available at this release tag.
"@ | Set-Content -LiteralPath $p1sNotesPath -Encoding utf8
    }
    & gh release create "v$Version" $p1sBinary $p1sManifest --repo $Repository --target $p1sCommit --title "Firmware $Version" --notes-file $p1sNotesPath
    if ($LASTEXITCODE -ne 0) { throw 'Release publication failed; existing releases were not overwritten.' }
} finally {
    Remove-Item -LiteralPath $p1sNotesPath -ErrorAction SilentlyContinue
}
