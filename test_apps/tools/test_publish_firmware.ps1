$ErrorActionPreference = 'Stop'
$p1sTestRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$p1sFixtureDir = Join-Path ([IO.Path]::GetTempPath()) ('p1s-publish-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $p1sFixtureDir | Out-Null
function global:p1s_fixture_python { $global:LASTEXITCODE = 0 }
function global:gh {
    $p1sArguments = @($args)
    $p1sState = $global:p1sPublishFixture
    $p1sState.calls.Add(($p1sArguments -join ' '))
    $global:LASTEXITCODE = 0
    if ($p1sArguments[0] -eq 'repo' -and $p1sArguments[1] -eq 'view') {
        if (-not $p1sState.exists) { $global:LASTEXITCODE = 1; return }
        if ($p1sArguments -contains '--jq') { return $p1sState.visibility }
        return (@{visibility=$p1sState.visibility;defaultBranchRef=$(if ($p1sState.initialized) {@{name='main'}} else {$null})} | ConvertTo-Json -Compress)
    }
    if ($p1sArguments[0] -eq 'repo' -and $p1sArguments[1] -eq 'create') {
        $p1sState.exists = $true
        $p1sState.initialized = $p1sArguments -contains '--add-readme'
        return
    }
    if ($p1sArguments[0] -eq 'api') {
        if ($p1sArguments -notcontains 'PUT' -or $p1sArguments -notcontains 'repos/fixture-owner/firmware/contents/README.md') { throw 'Unexpected publication API write.' }
        $p1sInputIndex = [Array]::IndexOf($p1sArguments, '--input') + 1
        $p1sBody = Get-Content -LiteralPath $p1sArguments[$p1sInputIndex] -Raw | ConvertFrom-Json
        $p1sReadme = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($p1sBody.content))
        if ($p1sReadme -notmatch 'compiled firmware' -or $p1sReadme -match 'password|C:/Users|BEGIN.*KEY') { throw 'Unexpected public README contents.' }
        $p1sState.initialized = $true
        return '{}'
    }
    if ($p1sArguments[0] -eq 'release' -and $p1sArguments[1] -eq 'create') {
        if (-not $p1sState.initialized) { $global:LASTEXITCODE = 1; return }
        if ($p1sArguments[3] -notlike '*p1s-heat-controller-esp32-4mb.bin' -or $p1sArguments[4] -notlike '*firmware.json') { throw 'Unexpected release assets.' }
        $p1sState.released = $true
        return
    }
    throw 'Unexpected gh operation.'
}
try {
    Set-Content -LiteralPath (Join-Path $p1sFixtureDir 'p1s-heat-controller-esp32-4mb.bin') -Value 'fixture'
    Set-Content -LiteralPath (Join-Path $p1sFixtureDir 'firmware.json') -Value '{}'
    foreach ($p1sCase in @('missing','empty','initialized','private')) {
        $global:p1sPublishFixture = @{exists=$p1sCase -ne 'missing';initialized=$p1sCase -eq 'initialized';
            visibility=$(if ($p1sCase -eq 'private') {'PRIVATE'} else {'PUBLIC'});released=$false;
            calls=[Collections.Generic.List[string]]::new()}
        $p1sRejected = $false
        try {
            & (Join-Path $p1sTestRoot 'tools/publish_firmware.ps1') -PackageDir $p1sFixtureDir -Version 1.0.0 -Repository fixture-owner/firmware -Python p1s_fixture_python -Publish | Out-Null
        } catch { if ($p1sCase -ne 'private') { throw }; $p1sRejected = $true }
        if ($p1sCase -eq 'private') {
            if (-not $p1sRejected -or $global:p1sPublishFixture.released) { throw 'Private repository was not rejected.' }
        } elseif (-not $global:p1sPublishFixture.released) { throw "Publication fixture failed: $p1sCase" }
        Write-Output "publisher $p1sCase repository: passed"
    }
} finally {
    $p1sResolved = (Resolve-Path -LiteralPath $p1sFixtureDir).Path
    if ([IO.Path]::GetDirectoryName($p1sResolved) -ne [IO.Path]::GetTempPath().TrimEnd('\')) { throw 'Unexpected fixture cleanup path.' }
    Remove-Item -LiteralPath $p1sResolved -Recurse -Force
    Remove-Item Function:/gh,Function:/p1s_fixture_python
    Remove-Variable -Name p1sPublishFixture -Scope Global
}
