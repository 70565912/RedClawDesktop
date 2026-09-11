param(
    [int]$Iterations = 1000,
    [int]$RetryBackoffMs = 75,
    [string]$ReportFile = "build/vs2022-x64/logs/m09-hardening-chaos-report.json",
    [switch]$BuildFirst
)

$ErrorActionPreference = "Stop"

if ($BuildFirst) {
    cmake --build --preset debug-local --target redclaw_session_recovery_chaos_runner
}

$runner = "build/vs2022-x64/tests/Debug/redclaw_session_recovery_chaos_runner.exe"
if (-not (Test-Path $runner)) {
    throw "Runner executable not found: $runner. Build with: cmake --build --preset debug-local --target redclaw_session_recovery_chaos_runner"
}

$reportDir = Split-Path -Parent $ReportFile
if ($reportDir -and -not (Test-Path $reportDir)) {
    New-Item -ItemType Directory -Path $reportDir | Out-Null
}

& $runner `
    --iterations $Iterations `
    --retry-backoff-ms $RetryBackoffMs `
    --report-file $ReportFile

if ($LASTEXITCODE -ne 0) {
    throw "M09 hardening chaos run failed with exit code $LASTEXITCODE"
}

Write-Host "M09 hardening chaos run completed. Report: $ReportFile"
