param([string[]]$Cases = @('source','initialized','missing','empty','private','dirty','existingtag','tagerror','stale','dryrun'))
$ErrorActionPreference = 'Stop'
$p1sTestRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$p1sFixtureDir = Join-Path ([IO.Path]::GetTempPath()) ('p1s-publish-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $p1sFixtureDir | Out-Null
$p1sPreviousIdf = $env:IDF_PATH
$env:IDF_PATH = $p1sFixtureDir
New-Item -ItemType Directory -Path (Join-Path $p1sFixtureDir 'tools'),(Join-Path $p1sFixtureDir 'build') | Out-Null
Set-Content -LiteralPath (Join-Path $p1sFixtureDir 'tools/idf.py') -Value '# fixture'
function global:p1s_fixture_python {
    $global:LASTEXITCODE = 0
    $p1sState = $global:p1sPublishFixture
    if ($args[0] -like '*idf.py') { $p1sState.built = $true }
    if ($args -contains '--verify-package' -and $args -contains '--build-dir') {
        if (-not $p1sState.built) { throw 'Package was checked before rebuilding the committed source.' }
        $p1sState.matched = $true
        if ($p1sState.stale) { $global:LASTEXITCODE = 1 }
    }
}
function global:git {
    $p1sArguments = @($args)
    $p1sState = $global:p1sPublishFixture
    $p1sState.gitCalls.Add(($p1sArguments -join ' '))
    $global:LASTEXITCODE = 0
    if ($p1sArguments -contains 'status') {
        if ($p1sState.dirty) { return ' M main/app_main.c' }
        return
    }
    if ($p1sArguments -contains 'rev-parse') { return $p1sState.commit }
    if ($p1sArguments -contains 'ls-remote') {
        if ($p1sState.tagerror) { $global:LASTEXITCODE = 1; return }
        if ($p1sState.existingtag) { return (('b' * 40) + ' refs/tags/v1.0.0') }
        return
    }
    throw 'Unexpected publisher Git command.'
}
function global:gh {
    $p1sArguments = @($args)
    $p1sState = $global:p1sPublishFixture
    $p1sState.calls.Add(($p1sArguments -join ' '))
    $global:LASTEXITCODE = 0
    if ($p1sArguments[0] -eq 'repo' -and $p1sArguments[1] -eq 'view') {
        if ($p1sArguments[2] -cne $p1sState.repository) { throw 'Publisher selected the wrong release repository.' }
        if (-not $p1sState.exists) { $global:LASTEXITCODE = 1; return }
        if ($p1sArguments -contains '--jq') { return $p1sState.visibility }
        return (@{visibility=$p1sState.visibility;defaultBranchRef=$(if ($p1sState.initialized) {@{name='main'}} else {$null})} | ConvertTo-Json -Compress)
    }
    if ($p1sArguments[0] -eq 'repo' -and $p1sArguments[1] -eq 'create') {
        throw 'Publisher must not create another repository.'
    }
    if ($p1sArguments[0] -eq 'api') {
        throw 'Publisher must not initialize or change source contents.'
    }
    if ($p1sArguments[0] -eq 'release' -and $p1sArguments[1] -eq 'create') {
        if (-not $p1sState.initialized) { $global:LASTEXITCODE = 1; return }
        if ($p1sArguments[3] -notlike '*p1s-heat-controller-esp32-4mb.bin' -or $p1sArguments[4] -notlike '*firmware.json') { throw 'Unexpected release assets.' }
        if ($p1sArguments -notcontains '--target') { throw 'Release tag must target the matching source commit.' }
        $p1sTargetIndex = [Array]::IndexOf($p1sArguments, '--target') + 1
        if ($p1sArguments[$p1sTargetIndex] -cne $p1sState.commit) { throw 'Release targets the wrong source commit.' }
        $p1sNotesIndex = [Array]::IndexOf($p1sArguments, '--notes-file') + 1
        $p1sNotes = Get-Content -LiteralPath $p1sArguments[$p1sNotesIndex] -Raw
        if ($p1sNotes -notmatch 'active-high' -or $p1sNotes -match 'active-low|BEGIN.*KEY|fixture-personal-password') { throw 'Release notes do not describe the selected public hardware profile.' }
        $p1sState.released = $true
        return
    }
    throw 'Unexpected gh operation.'
}
try {
    Set-Content -LiteralPath (Join-Path $p1sFixtureDir 'p1s-heat-controller-esp32-4mb.bin') -Value 'fixture'
    Set-Content -LiteralPath (Join-Path $p1sFixtureDir 'firmware.json') -Value '{}'
    foreach ($p1sCase in $Cases) {
        $p1sShouldReject = $p1sCase -in @('missing','empty','private','dirty','existingtag','tagerror','stale')
        $p1sUseDefault = $p1sCase -in @('source','dirty','dryrun')
        $global:p1sPublishFixture = @{exists=$p1sCase -ne 'missing';initialized=$p1sCase -ne 'empty';dirty=$p1sCase -eq 'dirty';
            existingtag=$p1sCase -eq 'existingtag';tagerror=$p1sCase -eq 'tagerror';stale=$p1sCase -eq 'stale';built=$false;matched=$false;
            visibility=$(if ($p1sCase -eq 'private') {'PRIVATE'} else {'PUBLIC'});released=$false;
            repository=$(if ($p1sUseDefault) {'ahmaddawod12/P1S-HeatControl'} else {'fixture-owner/firmware'});
            commit=('a' * 40);calls=[Collections.Generic.List[string]]::new();gitCalls=[Collections.Generic.List[string]]::new()}
        $p1sRejected = $false
        $p1sParams = @{PackageDir=$p1sFixtureDir;BuildDir=(Join-Path $p1sFixtureDir 'build');Version='1.0.0';Python='p1s_fixture_python';Publish=$p1sCase -ne 'dryrun'}
        if (-not $p1sUseDefault) { $p1sParams.Repository = 'fixture-owner/firmware' }
        try {
            & (Join-Path $p1sTestRoot 'tools/publish_firmware.ps1') @p1sParams | Out-Null
        } catch { if (-not $p1sShouldReject) { throw }; $p1sRejected = $true }
        if ($global:p1sPublishFixture.calls | Where-Object { $_ -match '^repo create |^api ' }) { throw 'Publisher attempted an unrelated repository mutation.' }
        if ($p1sCase -eq 'dryrun') {
            if ($global:p1sPublishFixture.calls.Count -or $global:p1sPublishFixture.gitCalls.Count -or $global:p1sPublishFixture.released) { throw 'Dry run must remain local and work before committing.' }
        } elseif ($p1sShouldReject) {
            if (-not $p1sRejected -or $global:p1sPublishFixture.released) { throw "Repository/source guard failed: $p1sCase" }
        } elseif (-not $global:p1sPublishFixture.released -or -not $global:p1sPublishFixture.built -or -not $global:p1sPublishFixture.matched) { throw "Publication fixture failed: $p1sCase" }
        Write-Output "publisher $p1sCase repository: passed"
    }
} finally {
    $env:IDF_PATH = $p1sPreviousIdf
    $p1sResolved = (Resolve-Path -LiteralPath $p1sFixtureDir).Path
    if ([IO.Path]::GetDirectoryName($p1sResolved) -ne [IO.Path]::GetTempPath().TrimEnd('\')) { throw 'Unexpected fixture cleanup path.' }
    Remove-Item -LiteralPath $p1sResolved -Recurse -Force
    Remove-Item Function:/gh,Function:/git,Function:/p1s_fixture_python
    Remove-Variable -Name p1sPublishFixture -Scope Global
}
