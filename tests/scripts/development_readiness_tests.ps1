param(
    [string]$OutputRoot = (Join-Path $PSScriptRoot '../../build/reports/development-readiness-tests')
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$root = Join-Path $OutputRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null

function Assert-True {
    param($Value, [string]$Message)
    if (-not $Value) { throw $Message }
}

function Write-Json {
    param($Value, [string]$Path)
    $Value | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Invoke-Case {
    param(
        [string]$Name,
        [long]$InputP95 = 150000,
        [long]$AckMaximum = 249999,
        [long]$HeartbeatMaximum = 499999,
        [double]$PressureFps = 15.0,
        [bool]$NativeSurface = $true,
        [bool]$Correctness = $true,
        [bool]$ExpectedPass = $true
    )

    $caseDirectory = Join-Path $root $Name
    New-Item -ItemType Directory -Path $caseDirectory | Out-Null
    $sourceGates = [ordered]@{
        schema = 'redclaw.local-performance-config-gates.v2'
        passed = $false
        native_size = $true
        gates = [ordered]@{
            at_least_three_rounds = $false
            complete_trace = $false
            diagnostic_integrity = $Correctness
            idle_recovery_duration = $true
            heartbeat = $false
            relative_fps = $false
            complete_text = $true
            decode_present = $true
            channels = $true
            input_lease = $true
            input_idle = $false
            input_pressure = $false
            ack_local_delivery = ($AckMaximum -lt 250000)
            native_surface = $NativeSurface
        }
        rounds = @([ordered]@{
            round = 1
            idle_fps = 20.0
            pressure_fps = $PressureFps
            recovery_fps = 15.0
        })
        inputs = [ordered]@{
            idle = [ordered]@{
                valid = $true
                unique_keyboard_batches = 100
                unique_mouse_batches = 100
                keyboard = @{ p95_us = $InputP95; max_us = $InputP95 }
                mouse = @{ p95_us = $InputP95; max_us = $InputP95 }
            }
            pressure = [ordered]@{
                valid = $true
                unique_keyboard_batches = 100
                unique_mouse_batches = 100
                keyboard = @{ p95_us = $InputP95; max_us = $InputP95 }
                mouse = @{ p95_us = $InputP95; max_us = $InputP95 }
            }
        }
        ack_local_maximum_us = $AckMaximum
    }
    Write-Json $sourceGates (Join-Path $caseDirectory 'source-gates.json')

    $trace = [ordered]@{
        valid = $true
        metric_contract = 'redclaw.gui-timing.v2'
        stages = @{ heartbeat_gap = @{ count = 100; max_us = $HeartbeatMaximum } }
    }
    $boundaries = @(
        [ordered]@{ window = '1-idle'; round_windows = @{ Host = $trace; Controller = $trace } },
        [ordered]@{ window = '1-pressure'; round_windows = @{ Host = $trace; Controller = $trace } },
        [ordered]@{ window = '1-recovered'; round_windows = @{ Host = $trace; Controller = $trace } }
    )
    Write-Json $boundaries (Join-Path $caseDirectory 'pressure-boundaries.json')
    Write-Json ([ordered]@{
        overall_passed = $true
        signal_transport = 'dht'
        runtime_sha256 = ('a' * 64)
    }) (Join-Path $caseDirectory 'run-result.json')

    $outputPath = Join-Path $caseDirectory 'result.json'
    $expectedSha256 = 'a' * 64
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $repo 'scripts/service/evaluate-development-readiness.ps1') `
        -GateReportPath (Join-Path $caseDirectory 'source-gates.json') `
        -RunResultPath (Join-Path $caseDirectory 'run-result.json') `
        -ExpectedRuntimeSha256 $expectedSha256 -OutputPath $outputPath *> `
        (Join-Path $caseDirectory 'evaluation.log')
    $exitCode = $LASTEXITCODE
    $result = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
    Assert-True ($result.passed -eq $ExpectedPass) "$Name result mismatch"
    Assert-True ($exitCode -eq $(if ($ExpectedPass) { 0 } else { 1 })) "$Name exit mismatch"
    Assert-True ($result.release_product_performance_status -eq 'unverified') `
        "$Name must not claim release qualification"
    if ($Correctness) {
        $expectedPerformance = $NativeSurface -and $InputP95 -le 150000 -and
            $AckMaximum -lt 250000 -and $HeartbeatMaximum -lt 500000 -and
            $PressureFps -ge 15.0
        Assert-True ($result.performance_within_calibration -eq $expectedPerformance) `
            "$Name performance observation mismatch"
    }
}

Invoke-Case -Name 'inclusive-input-and-fps-boundaries-pass'
Invoke-Case -Name 'exclusive-ack-boundary-warns' -AckMaximum 250000
Invoke-Case -Name 'exclusive-heartbeat-boundary-warns' -HeartbeatMaximum 500000
Invoke-Case -Name 'input-over-calibration-limit-warns' -InputP95 150001
Invoke-Case -Name 'fps-ratio-below-calibration-floor-warns' -PressureFps 14.99
Invoke-Case -Name 'hardware-surface-capability-does-not-block' -NativeSurface $false
Invoke-Case -Name 'correctness-remains-hard' -Correctness $false -ExpectedPass $false

Write-Output "PASS: correctness-only development readiness and non-blocking performance signals ($root)"
