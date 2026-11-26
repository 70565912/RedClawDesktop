param([string]$ReportRoot = ('build/reports/video-launch-' + [guid]::NewGuid().ToString('N')))

$ErrorActionPreference = 'Stop'
$supervisor = Join-Path $PSScriptRoot 'start-cross-lan-debug-supervisor.ps1'
$runtime = (Resolve-Path (Join-Path $PSScriptRoot '../../release/Debug/redclaw_desktop.exe')).Path
# Both DHT validators take a repository-relative report root.
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$fixtureDir = New-Item -ItemType Directory -Path (Join-Path $repoRoot $ReportRoot)
$iceFile = New-Item -ItemType File -Path (Join-Path $fixtureDir.FullName 'empty-ice.conf')
$dryRunOptions = @{ DryRun = $true; SessionCode = 'UNIT0001'; IceServerFile = $iceFile.FullName }
$cases = @(
    @{ Options = @{ Role = 'controller' }; Video = '' }
    @{ Options = @{ Role = 'host' }; Video = '--stream-require-capture' }
    @{ Options = @{ Role = 'controller'; AutoStart = $true }; Video = '' }
    @{ Options = @{ Role = 'host'; NoDesktopVideo = $true }; Video = '--no-desktop-video' }
    @{ Options = @{ Role = 'host'; RuntimeExe = $runtime; AutoStart = $true; NoDesktopVideo = $true }; Video = '--no-desktop-video' }
    @{ Options = @{ Role = 'controller'; NoDesktopVideo = $true }; Video = '--no-desktop-video' }
    @{ Options = @{ Role = 'controller'; StreamSmoke = $true }; Video = '--stream-smoke' }
    @{ Options = @{ Role = 'controller'; InputDiagnostics = $true }; Video = '--stream-smoke' }
    @{ Options = @{ Role = 'controller'; RuntimeExe = $runtime; AutoStart = $true }; Video = '--stream-smoke' }
    @{ Options = @{ Role = 'host'; RuntimeExe = $runtime }; Video = '--stream-require-capture' }
    @{ Options = @{ Role = 'controller'; RuntimeExe = $runtime; NoDesktopVideo = $true }; Video = '--no-desktop-video' }
)
foreach ($case in $cases) {
    $options = $case.Options
    $argv = (& $supervisor @options @dryRunOptions).Arguments
    $video = @($argv | Where-Object { $_ -in @('--runtime-config', '--desktop-video', '--no-desktop-video', '--stream-smoke', '--stream-require-capture') }) -join ' '
    if ($argv -notcontains '--ice-server-file' -or $video -ne $case.Video -or ($argv -contains '--gui-auto-start') -ne [bool]$options.AutoStart -or
        $argv[[array]::IndexOf($argv, '--gui-role') + 1] -ne $options.Role -or
        $argv[[array]::IndexOf($argv, '--run-seconds') + 1] -ne '0') { throw 'Incorrect final supervisor arguments.' }
}
foreach ($options in @(@{ StreamSmoke = $true }, @{ InputDiagnostics = $true })) {
    try {
        & $supervisor @options @dryRunOptions -Role host -NoDesktopVideo
        throw 'Conflicting diagnostic options unexpectedly accepted.'
    } catch {
        if ($_.Exception.Message -notlike 'NoDesktopVideo conflicts*') { throw }
    }
}
try {
    & $supervisor @dryRunOptions -RuntimeExe $runtime -Role host -NoDesktopVideo
    throw 'Manual Host Wait opt-out was accepted.'
} catch {
    if ($_.Exception.Message -notlike 'NoDesktopVideo with manual Host Wait and RuntimeExe is unsupported*') { throw }
}
foreach ($role in @('host', 'controller')) {
    & (Join-Path $PSScriptRoot "run-dht-$role-validation.ps1") -SessionCode UNIT0001 -RuntimeExe $runtime `
        -ConnectionCredentialFile $PSCommandPath -DryRun -SkipBootstrapReadiness -ReportRoot $reportRoot *> $null
    $result = Get-ChildItem -Path "$($fixtureDir.FullName)/dht-$role-validation-*/summary.json" | Get-Content -Raw | ConvertFrom-Json
    $argv = $result.arguments_redacted
    $profile = [array]::IndexOf($argv, '--runtime-config')
    if ($result.status -ne 'dry_run' -or $profile -lt 0 -or (Get-Content -Raw -LiteralPath $argv[$profile + 1]).Trim() -ne 'stream_smoke=false' -or
        @($argv | Where-Object { $_ -in @('--desktop-video', '--no-desktop-video', '--stream-smoke', '--stream-require-capture') }).Count) {
        throw "Incorrect final $role DHT arguments."
    }
}
Write-Output "Passed: $($cases.Count) assembled supervisor cases, 2 conflict cases, manual selected-Host refusal and 2 DHT dry-runs; no endpoints started. $reportRoot"
