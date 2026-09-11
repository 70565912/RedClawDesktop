param(
    [Parameter(Mandatory = $true)]
    [string]$GateReportPath,

    [Parameter(Mandatory = $true)]
    [string]$RunResultPath,

    [string]$OutputPath = '',

    [string]$ExpectedRuntimeSha256 = ''
)

$ErrorActionPreference = 'Stop'

$resolvedGatePath = (Resolve-Path -LiteralPath $GateReportPath).Path
$resolvedRunPath = (Resolve-Path -LiteralPath $RunResultPath).Path
$reportDirectory = Split-Path -Parent $resolvedGatePath
$boundaryPath = Join-Path $reportDirectory 'pressure-boundaries.json'
if (-not (Test-Path -LiteralPath $boundaryPath)) {
    throw "Pressure boundary evidence is missing: $boundaryPath"
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $reportDirectory 'development-readiness.json'
}
if (Test-Path -LiteralPath $OutputPath) {
    throw 'Preserve existing development-readiness evidence; choose a new OutputPath.'
}

$strict = Get-Content -LiteralPath $resolvedGatePath -Raw | ConvertFrom-Json
$run = Get-Content -LiteralPath $resolvedRunPath -Raw | ConvertFrom-Json
$boundaryDocument = Get-Content -LiteralPath $boundaryPath -Raw | ConvertFrom-Json
# Windows PowerShell 5.1 preserves a top-level JSON array as one pipeline
# object, while PowerShell 7 enumerates it. Normalize both hosts explicitly.
$boundaries = @($boundaryDocument | ForEach-Object { $_ })

if ($strict.schema -ne 'redclaw.local-performance-config-gates.v2') {
    throw "Unsupported source gate schema: $($strict.schema)"
}

$thresholds = [ordered]@{
    input_p95_us = 150000
    input_minimum_batches = 100
    classified_ack_max_us_exclusive = 250000
    running_heartbeat_max_us_exclusive = 500000
    successful_new_frame_minimum_fps = 12.0
    pressure_recovery_minimum_idle_ratio = 0.75
}

$rounds = @($strict.rounds)
$coveragePassed = $rounds.Count -eq 1 -and $boundaries.Count -eq 3
$requiredWindows = @('1-idle', '1-pressure', '1-recovered')
foreach ($windowName in $requiredWindows) {
    $coveragePassed = $coveragePassed -and @($boundaries | Where-Object window -EQ $windowName).Count -eq 1
}

$correctnessNames = @(
    'diagnostic_integrity',
    'idle_recovery_duration',
    'complete_text',
    'decode_present',
    'channels',
    'input_lease'
)
$correctness = [ordered]@{}
$correctnessPassed = $true
foreach ($name in $correctnessNames) {
    $value = [bool]($strict.gates.$name)
    $correctness[$name] = $value
    $correctnessPassed = $correctnessPassed -and $value
}
$nativeSurfaceCapability = $null
if ($null -ne $strict.gates.native_surface) {
    $nativeSurfaceCapability = [bool]($strict.gates.native_surface)
}

$heartbeatMaximumUs = 0L
$heartbeatSamples = 0L
$heartbeatPassed = $coveragePassed
foreach ($boundary in $boundaries) {
    foreach ($role in @('Host', 'Controller')) {
        $window = $boundary.round_windows.$role
        $sample = $window.stages.heartbeat_gap
        $classified = $window.valid -and
            $window.metric_contract -eq 'redclaw.gui-timing.v2' -and
            $null -ne $sample -and [long]($sample.count) -gt 0
        $heartbeatPassed = $heartbeatPassed -and $classified
        if ($classified) {
            $heartbeatSamples += [long]($sample.count)
            if ([long]($sample.max_us) -gt $heartbeatMaximumUs) {
                $heartbeatMaximumUs = [long]($sample.max_us)
            }
            $heartbeatPassed = $heartbeatPassed -and
                [long]($sample.max_us) -lt $thresholds.running_heartbeat_max_us_exclusive
        }
    }
}

$fpsPassed = $coveragePassed
$fpsObservations = @()
foreach ($round in $rounds) {
    $idle = [double]($round.idle_fps)
    $pressure = [double]($round.pressure_fps)
    $recovery = [double]($round.recovery_fps)
    $pressureRatio = if ($idle -gt 0) { $pressure / $idle } else { 0.0 }
    $recoveryRatio = if ($idle -gt 0) { $recovery / $idle } else { 0.0 }
    $roundPassed = $idle -ge $thresholds.successful_new_frame_minimum_fps -and
        $pressure -ge $thresholds.successful_new_frame_minimum_fps -and
        $recovery -ge $thresholds.successful_new_frame_minimum_fps -and
        $pressureRatio -ge $thresholds.pressure_recovery_minimum_idle_ratio -and
        $recoveryRatio -ge $thresholds.pressure_recovery_minimum_idle_ratio
    $fpsPassed = $fpsPassed -and $roundPassed
    $fpsObservations += [ordered]@{
        round = [int]($round.round)
        idle_fps = $idle
        pressure_fps = $pressure
        recovery_fps = $recovery
        pressure_ratio = $pressureRatio
        recovery_ratio = $recoveryRatio
        within_calibration = $roundPassed
    }
}

$inputPassed = $true
$inputObservations = [ordered]@{}
foreach ($phase in @('idle', 'pressure')) {
    $sample = $strict.inputs.$phase
    $phasePassed = $null -ne $sample -and [bool]($sample.valid) -and
        [long]($sample.unique_keyboard_batches) -ge $thresholds.input_minimum_batches -and
        [long]($sample.unique_mouse_batches) -ge $thresholds.input_minimum_batches -and
        [long]($sample.keyboard.p95_us) -le $thresholds.input_p95_us -and
        [long]($sample.mouse.p95_us) -le $thresholds.input_p95_us
    $inputPassed = $inputPassed -and $phasePassed
    $inputObservations[$phase] = [ordered]@{
        keyboard_batches = [long]($sample.unique_keyboard_batches)
        mouse_batches = [long]($sample.unique_mouse_batches)
        keyboard_p95_us = [long]($sample.keyboard.p95_us)
        mouse_p95_us = [long]($sample.mouse.p95_us)
        keyboard_max_us = [long]($sample.keyboard.max_us)
        mouse_max_us = [long]($sample.mouse.max_us)
        within_calibration = $phasePassed
    }
}

$ackMaximumUs = [long]($strict.ack_local_maximum_us)
$ackPassed = [bool]($strict.gates.ack_local_delivery) -and $ackMaximumUs -gt 0 -and
    $ackMaximumUs -lt $thresholds.classified_ack_max_us_exclusive

$runtimeSha256 = [string]($run.runtime_sha256)
$identityPassed = [bool]($run.overall_passed) -and $run.signal_transport -eq 'dht' -and
    $runtimeSha256 -match '^[0-9a-fA-F]{64}$'
if (-not [string]::IsNullOrWhiteSpace($ExpectedRuntimeSha256)) {
    $identityPassed = $identityPassed -and
        $runtimeSha256.Equals($ExpectedRuntimeSha256, [StringComparison]::OrdinalIgnoreCase)
}

$performanceSignals = [ordered]@{
    one_complete_calibration_round = $coveragePassed
    native_surface_capability = $nativeSurfaceCapability
    input_latency_within_calibration = $inputPassed
    classified_ack_within_calibration = $ackPassed
    running_gui_heartbeat_within_calibration = $heartbeatPassed
    successful_new_frame_fps_within_calibration = $fpsPassed
}
$performanceWithinCalibration = @(
    $performanceSignals.Values | Where-Object { $null -ne $_ -and -not $_ }
).Count -eq 0

$gates = [ordered]@{
    artifact_and_real_path_identity = $identityPassed
    correctness = $correctnessPassed
}
$passed = @($gates.Values | Where-Object { -not $_ }).Count -eq 0

$result = [ordered]@{
    schema = 'redclaw.development-readiness.v1'
    gate_id = 'DEV-FUNC-1'
    passed = $passed
    decision_scope = 'feature-development-continuation'
    blocking_policy = 'correctness-only'
    release_product_performance_status = 'unverified'
    release_qualification = $false
    source = [ordered]@{
        strict_gate_path = $resolvedGatePath
        strict_gate_sha256 = (Get-FileHash -LiteralPath $resolvedGatePath -Algorithm SHA256).Hash.ToLowerInvariant()
        run_result_path = $resolvedRunPath
        run_result_sha256 = (Get-FileHash -LiteralPath $resolvedRunPath -Algorithm SHA256).Hash.ToLowerInvariant()
        runtime_sha256 = $runtimeSha256.ToLowerInvariant()
        signal_transport = [string]($run.signal_transport)
    }
    thresholds = $thresholds
    gates = $gates
    correctness = $correctness
    performance_signals = $performanceSignals
    performance_within_calibration = $performanceWithinCalibration
    strict_debug_performance_passed = [bool]($strict.passed)
    observed = [ordered]@{
        heartbeat_sample_count = $heartbeatSamples
        heartbeat_maximum_us = $heartbeatMaximumUs
        classified_ack_maximum_us = $ackMaximumUs
        inputs = $inputObservations
        fps = $fpsObservations
    }
    interpretation = 'Passing permits feature development to continue because correctness and real-path identity pass. Performance signals are non-blocking observations and do not satisfy PB-REL-1.'
}

$result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
$gates | ConvertTo-Json
if (-not $passed) {
    exit 1
}
