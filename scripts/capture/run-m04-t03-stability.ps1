param(
    [int]$DurationSeconds = 1800,
    [int]$OutputIndex = 0,
    [int]$FrameTimeoutMs = 250,
    [int]$MaxConsecutiveTimeouts = 120,
    [int]$MaxConsecutiveFailures = 5,
    [string]$ReportFile = "build/vs2022-x64/logs/m04-t03-stability-report.json",
    [switch]$BuildFirst
)

$ErrorActionPreference = "Stop"

if ($BuildFirst) {
    cmake --build --preset debug-local --target redclaw_capture_stability_runner
}

$runner = "build/vs2022-x64/tests/Debug/redclaw_capture_stability_runner.exe"
if (-not (Test-Path $runner)) {
    throw "Runner executable not found: $runner. Build with: cmake --build --preset debug-local --target redclaw_capture_stability_runner"
}

$reportDir = Split-Path -Parent $ReportFile
if ($reportDir -and -not (Test-Path $reportDir)) {
    New-Item -ItemType Directory -Path $reportDir | Out-Null
}

& $runner `
    --duration-seconds $DurationSeconds `
    --output-index $OutputIndex `
    --frame-timeout-ms $FrameTimeoutMs `
    --max-consecutive-timeouts $MaxConsecutiveTimeouts `
    --max-consecutive-failures $MaxConsecutiveFailures `
    --report-file $ReportFile

if ($LASTEXITCODE -ne 0) {
    throw "M04-T03 stability run failed with exit code $LASTEXITCODE"
}

Write-Host "M04-T03 stability run completed. Report: $ReportFile"
