param([string]$ReportRoot = 'build/reports/debug-runtime-freshness')

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runRoot = Join-Path $repoRoot (Join-Path $ReportRoot (Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
$testPowerShell = (Get-Process -Id $PID).Path
$results = [System.Collections.Generic.List[object]]::new()

function New-Fixture {
    param([string]$Name)
    $root = Join-Path $runRoot $Name
    foreach ($directory in @('scripts/service', 'release/Debug', 'src', 'build')) {
        New-Item -ItemType Directory -Path (Join-Path $root $directory) -Force | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $repoRoot 'build.ps1') -Destination $root
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'prepare-dht-remote-validation.ps1') -Destination (Join-Path $root 'scripts/service')
    foreach ($name in @('run-dht-role-validation-workflow.ps1', 'summarize-dht-workflow-result.ps1', 'test-dht-bootstrap-readiness.ps1')) {
        Set-Content -LiteralPath (Join-Path $root "scripts/service/$name") -Value '# fixture'
    }
    # Exercise the real build wrapper and prep together, replacing only expensive
    # build/publication operations. The no-op builder preserves the EXE timestamp.
    @'
param([string]$Configuration)
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Add-Content -LiteralPath (Join-Path $root 'build/invocations.txt') -Value 'build'
if (Test-Path (Join-Path $root 'fail-build')) { throw 'fixture build failure' }
if (Test-Path (Join-Path $root 'change-during-build')) {
    Start-Sleep -Milliseconds 50
    Add-Content -LiteralPath (Join-Path $root 'src/main.cpp') -Value '// concurrent change'
}
$global:LASTEXITCODE = 0
'@ | Set-Content -LiteralPath (Join-Path $root 'scripts/service/build-desktop.ps1')
    @'
param([string]$Configuration)
if (Test-Path (Join-Path $PSScriptRoot 'fail-publish')) { throw 'fixture publication failure' }
$global:LASTEXITCODE = 0
'@ | Set-Content -LiteralPath (Join-Path $root 'publish.ps1')
    Set-Content -LiteralPath (Join-Path $root 'release/Debug/redclaw_desktop.exe') -Value 'fixture executable bytes'
    # Each fixture owns fake runtime bytes and stubbed build/publication. A real
    # serving GUI outside this fixture must not trip its process-occupation gate.
    @'
param([switch]$BuildIfStale)
function Get-Process {
    [CmdletBinding()]
    param([string]$Name)
    if ($Name -eq 'redclaw_desktop') { return }
    Microsoft.PowerShell.Management\Get-Process -Name $Name
}
& (Join-Path $PSScriptRoot 'scripts/service/prepare-dht-remote-validation.ps1') -Json -BuildIfStale:$BuildIfStale
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath (Join-Path $root 'run-prep.ps1')
    (Get-Item (Join-Path $root 'release/Debug/redclaw_desktop.exe')).LastWriteTimeUtc = [DateTime]::UtcNow.AddDays(-2)
    foreach ($source in @('CMakeLists.txt', 'src/main.cpp')) {
        Set-Content -LiteralPath (Join-Path $root $source) -Value '// input'
        (Get-Item (Join-Path $root $source)).LastWriteTimeUtc = [DateTime]::UtcNow.AddMinutes(-1)
    }
    return $root
}

function Invoke-FixturePrep {
    param([string]$Root, [switch]$BuildIfStale)
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Root 'run-prep.ps1'))
    if ($BuildIfStale) { $arguments += '-BuildIfStale' }
    # Expected fixture failures write stderr. Windows PowerShell 5 converts
    # redirected native stderr to errors; retain its exit code for assertions.
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $testPowerShell @arguments *> (Join-Path $Root 'prep.log')
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
}

function Assert-Case {
    param([string]$Name, [bool]$Condition)
    $results.Add([pscustomobject]@{ name = $Name; passed = $Condition })
    Write-Host ("[{0}] {1}" -f $(if ($Condition) { 'PASS' } else { 'FAIL' }), $Name)
    if (-not $Condition) { throw "Regression failed: $Name; evidence: $runRoot" }
}

try {
    $fixture = New-Fixture 'noop-build'
    $runtime = Join-Path $fixture 'release/Debug/redclaw_desktop.exe'
    $originalTime = (Get-Item $runtime).LastWriteTimeUtc
    Assert-Case 'stale runtime rejected without build' ((Invoke-FixturePrep $fixture) -ne 0)
    Assert-Case 'successful no-op build permits startup' ((Invoke-FixturePrep $fixture -BuildIfStale) -eq 0)
    Assert-Case 'executable timestamp preserved' ((Get-Item $runtime).LastWriteTimeUtc -eq $originalTime)
    Assert-Case 'next launch accepts verified inputs' ((Invoke-FixturePrep $fixture -BuildIfStale) -eq 0)
    Assert-Case 'next launch avoids another build' (@(Get-Content (Join-Path $fixture 'build/invocations.txt')).Count -eq 1)
    $receiptPath = Join-Path $fixture 'build/runtime-build-Debug.json'
    $validReceipt = Get-Content $receiptPath -Raw

    Set-Content -LiteralPath $runtime -Value 'replacement executable'
    (Get-Item $runtime).LastWriteTimeUtc = $originalTime
    Assert-Case 'replacement bytes invalidate receipt' ((Invoke-FixturePrep $fixture) -ne 0)
    Set-Content -LiteralPath $runtime -Value 'fixture executable bytes'
    (Get-Item $runtime).LastWriteTimeUtc = $originalTime
    Set-Content -LiteralPath $receiptPath -Value '{broken'
    Assert-Case 'malformed receipt cannot approve stale runtime' ((Invoke-FixturePrep $fixture) -ne 0)
    $receipt = $validReceipt | ConvertFrom-Json
    $receipt.configuration = 'Release'
    $receipt | ConvertTo-Json | Set-Content $receiptPath
    Assert-Case 'other configuration receipt rejected' ((Invoke-FixturePrep $fixture) -ne 0)
    $receipt.configuration = 'Debug'
    $receipt.build_started_at_utc = [DateTime]::UtcNow.AddDays(1).ToString('o')
    $receipt | ConvertTo-Json | Set-Content $receiptPath
    Assert-Case 'future receipt rejected' ((Invoke-FixturePrep $fixture) -ne 0)
    Set-Content -LiteralPath $receiptPath -Value $validReceipt
    Add-Content -LiteralPath (Join-Path $fixture 'src/main.cpp') -Value '// newer source'
    Assert-Case 'source edit after build rejected' ((Invoke-FixturePrep $fixture) -ne 0)

    foreach ($failure in @('fail-build', 'fail-publish')) {
        $fixture = New-Fixture $failure
        Set-Content -LiteralPath (Join-Path $fixture $failure) -Value 'enabled'
        Assert-Case "$failure blocks startup" ((Invoke-FixturePrep $fixture -BuildIfStale) -ne 0)
        Assert-Case "$failure creates no successful receipt" (-not (Test-Path (Join-Path $fixture 'build/runtime-build-Debug.json')))
    }
    $fixture = New-Fixture 'change-during-build'
    Set-Content -LiteralPath (Join-Path $fixture 'change-during-build') -Value 'enabled'
    Assert-Case 'source edit during build still rejected' ((Invoke-FixturePrep $fixture -BuildIfStale) -ne 0)

    $fixture = New-Fixture 'current-runtime'
    (Get-Item (Join-Path $fixture 'release/Debug/redclaw_desktop.exe')).LastWriteTimeUtc = [DateTime]::UtcNow
    Assert-Case 'current runtime needs no receipt or build' ((Invoke-FixturePrep $fixture -BuildIfStale) -eq 0 -and -not (Test-Path (Join-Path $fixture 'build/invocations.txt')))
} finally {
    $results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding UTF8
    Write-Host "Evidence: $runRoot"
}
