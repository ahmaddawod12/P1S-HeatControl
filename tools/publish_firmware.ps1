param(
    [Parameter(Mandatory=$true)][string]$PackageDir,
    [Parameter(Mandatory=$true)][string]$Version,
    [string]$Repository = 'ahmaddawod12/P1S-HeatControl-Releases',
    [string]$Python = 'python',
    [switch]$Publish
)
$ErrorActionPreference = 'Stop'
$p1sRoot = Split-Path -Parent $PSScriptRoot
$p1sPackage = (Resolve-Path -LiteralPath $PackageDir).Path
if ($Repository -notmatch '^[A-Za-z0-9-]+/[A-Za-z0-9_.-]+$' -or $Repository -ieq 'ahmaddawod12/P1S-HeatControl') {
    throw 'Use a separate public firmware repository; never publish private source.'
}
& $Python (Join-Path $PSScriptRoot 'package_firmware.py') --verify-package $p1sPackage --public-key (Join-Path $p1sRoot 'config/ota_signing_public.bin') --version $Version | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Public package validation failed.' }
$p1sBinary = Join-Path $p1sPackage 'p1s-heat-controller-esp32-4mb.bin'
$p1sManifest = Join-Path $p1sPackage 'firmware.json'
Write-Output "Verified public release: $Repository, tag v$Version"
Get-Item -LiteralPath $p1sBinary,$p1sManifest | Select-Object Name,Length
if (-not $Publish) {
    Write-Output 'No repository or release was changed. Add -Publish to publish these two files.'
    exit 0
}
$p1sRepositoryInfo = & gh repo view $Repository --json visibility,defaultBranchRef 2>$null
if ($LASTEXITCODE -ne 0) {
    & gh repo create $Repository --public --add-readme --description 'Signed firmware releases for the P1S chamber heat controller'
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the public firmware repository.' }
    $p1sRepositoryInfo = & gh repo view $Repository --json visibility,defaultBranchRef
    if ($LASTEXITCODE -ne 0) { throw 'Could not verify the created firmware repository.' }
}
$p1sRepositoryState = $p1sRepositoryInfo | ConvertFrom-Json
if ($p1sRepositoryState.visibility -ne 'PUBLIC') { throw 'The selected release repository must already be public; visibility was not changed.' }
if (-not $p1sRepositoryState.defaultBranchRef) {
    # A release tag needs a commit. Initialize an existing empty distribution
    # repository with fixed public prose, never this checkout's Git history.
    $p1sReadme = "# P1S firmware releases`n`nSigned compiled firmware for the P1S chamber heat controller. Install updates from the local dashboard after the initial USB setup.`n"
    $p1sInitPath = Join-Path ([IO.Path]::GetTempPath()) ('p1s-release-init-' + [Guid]::NewGuid().ToString('N') + '.json')
    try {
        @{message='Initialize firmware distribution';content=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($p1sReadme))} |
            ConvertTo-Json | Set-Content -LiteralPath $p1sInitPath -Encoding utf8
        & gh api "repos/$Repository/contents/README.md" --method PUT --input $p1sInitPath | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the empty distribution repository.' }
    } finally { Remove-Item -LiteralPath $p1sInitPath -ErrorAction SilentlyContinue }
}
$p1sNotesPath = Join-Path ([IO.Path]::GetTempPath()) ('p1s-release-' + [Guid]::NewGuid().ToString('N') + '.md')
try {
    @"
Signed firmware $Version for the classic ESP32-WROOM-32 P1S controller with 4 MB flash and active-low relays.

Use the dashboard's Update firmware button after the initial USB OTA bootstrap. Heating stays disabled after installation. firmware.json describes the signed application and its SHA-256 digest.

This release contains compiled firmware only. Wi-Fi credentials are retained on each device.
"@ | Set-Content -LiteralPath $p1sNotesPath -Encoding utf8
    & gh release create "v$Version" $p1sBinary $p1sManifest --repo $Repository --title "Firmware $Version" --notes-file $p1sNotesPath
    if ($LASTEXITCODE -ne 0) { throw 'Release publication failed; existing releases were not overwritten.' }
} finally {
    Remove-Item -LiteralPath $p1sNotesPath -ErrorAction SilentlyContinue
}
