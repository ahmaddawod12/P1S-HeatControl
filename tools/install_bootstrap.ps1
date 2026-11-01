param(
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [Parameter(Mandatory=$true)][string]$PrivateKey,
    [string]$Port,
    [string]$Python = 'python',
    [ValidateSet(115200,230400,460800)][int]$Baud = 115200,
    [switch]$Flash
)
$ErrorActionPreference = 'Stop'
$p1sRoot = Split-Path -Parent $PSScriptRoot
$p1sBuild = (Resolve-Path -LiteralPath $BuildDir).Path
$p1sKey = (Resolve-Path -LiteralPath $PrivateKey).Path
if ($p1sKey.StartsWith($p1sRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Keep the private signing key outside the source checkout.'
}
$p1sDescription = Get-Content -LiteralPath (Join-Path $p1sBuild 'project_description.json') -Raw | ConvertFrom-Json
$p1sVersion = $p1sDescription.project_version
if ($p1sDescription.app_bin -ne 'p1s_heat_controller.bin') { throw 'Unexpected bootstrap application.' }
$p1sSigned = Join-Path $p1sBuild 'p1s-heat-controller-esp32-4mb.bin'
& $Python -m espsecure sign-data --version 1 --keyfile $p1sKey --output $p1sSigned (Join-Path $p1sBuild 'p1s_heat_controller.bin')
if ($LASTEXITCODE -ne 0) { throw 'Bootstrap signing failed.' }
& $Python (Join-Path $PSScriptRoot 'package_firmware.py') --check-bootstrap $p1sBuild --public-key (Join-Path $p1sRoot 'config/ota_signing_public.bin') --version $p1sVersion | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Bootstrap validation failed; nothing was flashed.' }
$p1sFlashArgs = Get-Content -LiteralPath (Join-Path $p1sBuild 'flasher_args.json') -Raw | ConvertFrom-Json
if ($p1sFlashArgs.extra_esptool_args.chip -ne 'esp32' -or $p1sFlashArgs.flash_settings.flash_size -ne '4MB') {
    throw 'Unexpected chip or flash geometry.'
}
$p1sExpected = @{'0x1000'='bootloader/bootloader.bin'; '0x8000'='partition_table/partition-table.bin'; '0x16000'='ota_data_initial.bin'; '0x20000'='p1s_heat_controller.bin'}
$p1sProperties = @($p1sFlashArgs.flash_files.PSObject.Properties)
if ($p1sProperties.Count -ne $p1sExpected.Count) { throw 'Unexpected flash write set.' }
foreach ($p1sProperty in $p1sProperties) {
    if (-not $p1sExpected.ContainsKey($p1sProperty.Name) -or ($p1sProperty.Value -replace '\\','/') -ne $p1sExpected[$p1sProperty.Name]) {
        throw 'Flash offsets/files do not match the approved OTA layout.'
    }
}
Write-Output "Prepared and verified personal bootstrap: $p1sSigned"
Write-Output 'This image carries Wi-Fi credentials. Keep it local. Public release packaging rejects it.'
if (-not $Flash) {
    Write-Output 'No board was changed. Add -Port COMx -Flash after the unloaded-relay commissioning checks.'
    exit 0
}
if ($Port -notmatch '^COM[1-9][0-9]*$') { throw 'Select the intended Windows serial port explicitly.' }
$p1sPortInfo = Get-CimInstance Win32_SerialPort | Where-Object DeviceID -EQ $Port
if (-not $p1sPortInfo) { throw 'The selected serial port is not present.' }
$p1sBackupDir = Join-Path $p1sBuild 'private-backups'
New-Item -ItemType Directory -Force -Path $p1sBackupDir | Out-Null
$p1sBackup = Join-Path $p1sBackupDir ('nvs-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '.bin')
& $Python -m esptool --chip esp32 --port $Port --baud $Baud --before default-reset --after no-reset read-flash 0x9000 0x6000 $p1sBackup
if ($LASTEXITCODE -ne 0) { throw 'Settings backup failed; flash was not erased.' }
& $Python -m esptool --chip esp32 --port $Port --before default-reset --after no-reset erase-region 0x10000 0x8000
if ($LASTEXITCODE -ne 0) { throw 'New device configuration/OTA data preparation failed. Keep USB connected for recovery.' }
Push-Location $p1sBuild
try {
    $p1sWrite = @('-m','esptool','--chip','esp32','--port',$Port,'--baud',"$Baud",'--before','default-reset','--after','hard-reset','write-flash')
    $p1sWrite += @('--flash-mode', $p1sFlashArgs.flash_settings.flash_mode, '--flash-freq', $p1sFlashArgs.flash_settings.flash_freq, '--flash-size', '4MB')
    $p1sWrite += @('0x1000','bootloader/bootloader.bin','0x8000','partition_table/partition-table.bin','0x16000','ota_data_initial.bin','0x20000',$p1sSigned)
    & $Python @p1sWrite
    if ($LASTEXITCODE -ne 0) { throw 'USB installation failed. Keep USB connected and retry the verified bootstrap.' }
} finally { Pop-Location }
Write-Output "USB installation complete. Existing settings were retained; backup: $p1sBackup"
Write-Output 'Open http://p1s-heat.local and wait for initial confirmation before testing OTA.'
