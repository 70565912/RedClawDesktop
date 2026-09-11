param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$ReportRoot = 'build\reports',

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

function Invoke-CapturedNativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList,

        [Parameter(Mandatory = $true)]
        [string]$OutputPath
    )

    $output = @(& $FilePath @ArgumentList 2>&1)
    $exitCode = $LASTEXITCODE
    $output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
    foreach ($line in $output) {
        Write-Host $line
    }
    return $exitCode
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$runId = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$reportRootAbsolute = if ([System.IO.Path]::IsPathRooted($ReportRoot)) {
    $ReportRoot
} else {
    Join-Path $repoRoot $ReportRoot
}
$reportDirectory = Join-Path $reportRootAbsolute "local-stream-simulation-$runId"
New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null
$buildLog = Join-Path $reportDirectory 'build.log'
$testLog = Join-Path $reportDirectory 'ctest.log'
$resultJson = Join-Path $reportDirectory 'result.json'
$testDirectory = @(
    (Join-Path $repoRoot 'build\ninja-x64'),
    (Join-Path $repoRoot 'build\vs2022-x64')
) | Where-Object {
    Test-Path -LiteralPath (Join-Path $_ 'CTestTestfile.cmake')
} | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($testDirectory)) {
    throw 'No configured CTest build directory was found under build\ninja-x64 or build\vs2022-x64.'
}
$cmakeCachePath = Join-Path $testDirectory 'CMakeCache.txt'
$cmakeCommandEntry = Get-Content -LiteralPath $cmakeCachePath | Where-Object {
    $_ -like 'CMAKE_COMMAND:INTERNAL=*'
} | Select-Object -First 1
$ctestExecutable = $null
if (-not [string]::IsNullOrWhiteSpace($cmakeCommandEntry)) {
    $cmakeExecutable = $cmakeCommandEntry.Substring($cmakeCommandEntry.IndexOf('=') + 1)
    $configuredCtest = Join-Path (Split-Path -Parent $cmakeExecutable) 'ctest.exe'
    if (Test-Path -LiteralPath $configuredCtest) {
        $ctestExecutable = $configuredCtest
    }
}
if ([string]::IsNullOrWhiteSpace($ctestExecutable)) {
    $ctestCommand = Get-Command 'ctest' -ErrorAction SilentlyContinue
    if ($null -ne $ctestCommand) {
        $ctestExecutable = $ctestCommand.Source
    }
}
if ([string]::IsNullOrWhiteSpace($ctestExecutable)) {
    throw "CTest executable was not found for configured build directory: $testDirectory"
}

$buildExitCode = 0
if (-not $SkipBuild) {
    $buildPreset = if ($Configuration -eq 'Debug') { 'debug-local' } else { 'release-local' }
    $guardedCmakeScript = Join-Path $repoRoot 'scripts\service\run-cmake-guarded.ps1'
    $cmakeArguments = @(
        '--build', '--preset', $buildPreset,
        '--target',
        'redclaw_net_video_frame_transport_tests',
        'redclaw_service_dht_rendezvous_tests',
        'redclaw_service_connection_negotiation_tests',
        'redclaw_ffmpeg_roundtrip_integration_tests',
        'redclaw_desktop_stream_simulation_integration_tests'
    )
    $encodedCmakeArguments = [Convert]::ToBase64String(
        [System.Text.Encoding]::UTF8.GetBytes(
            [string]::Join([char]31, $cmakeArguments)))
    Write-Host "[stream-simulation] building focused test targets with preset=$buildPreset"
    $buildExitCode = Invoke-CapturedNativeCommand `
        -FilePath 'powershell.exe' `
        -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $guardedCmakeScript,
            '-CMakeArgsBase64', $encodedCmakeArguments
        ) `
        -OutputPath $buildLog
}

$testExitCode = -1
if ($buildExitCode -eq 0) {
    Write-Host '[stream-simulation] running deterministic media and DHT matrix'
    $testExitCode = Invoke-CapturedNativeCommand `
        -FilePath $ctestExecutable `
        -ArgumentList @(
            '--test-dir', $testDirectory,
            '-C', $Configuration,
            '--output-on-failure',
            '-R', '^(redclaw_net_video_frame_transport_tests|redclaw_service_dht_rendezvous_tests|redclaw_service_connection_negotiation_tests|redclaw_ffmpeg_roundtrip_integration_tests|redclaw_desktop_stream_simulation_integration_tests)$'
        ) `
        -OutputPath $testLog
}

$scenarios = @(
    [ordered]@{
        id = 'media_static_control_alive'
        coverage = 'one retained IDR, zero new media, live control heartbeat, stale feedback rejection'
    },
    [ordered]@{
        id = 'remote_log_after_control_reconnect'
        coverage = 'bounded remote-log request is restamped for a replacement control epoch and completes without accepting old-epoch replay'
    },
    [ordered]@{
        id = 'viewport_bounded_replay_after_control_reconnect'
        coverage = 'sticky viewport request is restamped and accepted repeatedly in the replacement control epoch'
    },
    [ordered]@{
        id = 'media_forced_active'
        coverage = 'continuous changed frames through real fragment serialization, feedback and reassembly'
    },
    [ordered]@{
        id = 'media_seeded_random_fragment_loss'
        coverage = 'deterministic random loss, dependent-frame discard and latest complete IDR recovery'
    },
    [ordered]@{
        id = 'media_weak_network'
        coverage = 'bounded token-bucket throughput and pressure backoff'
    },
    [ordered]@{
        id = 'media_capacity_step_down_up'
        coverage = 'abrupt congestion reduction followed by stable-window probing recovery'
    },
    [ordered]@{
        id = 'media_transport_only_startup_stall'
        coverage = 'open ICE/data channels with captured frames but no first encoded/transmitted frame fails at a bounded deadline and clears only after real media delivery'
    },
    [ordered]@{
        id = 'feedback_revision_and_age_isolation'
        coverage = 'old revision retires only old in-flight metadata and cannot alter current rate'
    },
    [ordered]@{
        id = 'dht_publish_loss_fetch_miss_delay'
        coverage = 'offer, answer and acknowledgement convergence with deterministic DHT faults'
    },
    [ordered]@{
        id = 'dht_out_of_order_revision'
        coverage = 'late old record cannot roll back revision or ICE generation'
    },
    [ordered]@{
        id = 'dht_latest_state_candidate_monotonicity'
        coverage = 'answer acknowledgement and direct-candidate revisions retain every candidate from the previously published same-generation snapshot'
    },
    [ordered]@{
        id = 'dht_ice_negotiation_reordering_and_recovery'
        coverage = 'persistent offer, duplicate/reordered delivery, late candidates and bounded reconnect state transitions'
    },
    [ordered]@{
        id = 'dht_established_session_persistent_reconnect'
        coverage = 'post-session Controller reconnect backoff reaches 16 seconds and remains active beyond the fifth failed generation'
    },
    [ordered]@{
        id = 'dht_failed_generation_not_reused'
        coverage = 'Controller publishes a fresh repair request without re-adopting the failed Host generation; Host replaces an answered unconnected generation immediately'
    },
    [ordered]@{
        id = 'dht_completed_offer_not_reused_by_fresh_controller'
        coverage = 'a newly started Controller skips a Host offer whose answer acknowledgement proves that its ICE credentials were already consumed'
    },
    [ordered]@{
        id = 'dht_host_post_session_request_takeover'
        coverage = 'healthy Host lease rejects competitors, then post-disconnect standby immediately accepts a fresh Controller request and advances the Host-owned generation'
    },
    [ordered]@{
        id = 'ffmpeg_low_latency_roundtrip'
        coverage = 'real encode/decode payloads and decoder reset across three resolutions'
    }
)

$overallPassed = $buildExitCode -eq 0 -and $testExitCode -eq 0
$result = [ordered]@{
    schema = 'redclaw.local-stream-simulation.result.v1'
    generated_at = (Get-Date).ToString('o')
    configuration = $Configuration
    overall_passed = $overallPassed
    build_skipped = [bool]$SkipBuild
    build_exit_code = $buildExitCode
    test_exit_code = $testExitCode
    test_directory = $testDirectory
    ctest_executable = $ctestExecutable
    deterministic_seed = '0x5EED1234'
    scenario_count = $scenarios.Count
    scenarios = $scenarios
    build_log = $(if (Test-Path -LiteralPath $buildLog) { $buildLog } else { '' })
    test_log = $(if (Test-Path -LiteralPath $testLog) { $testLog } else { '' })
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resultJson -Encoding UTF8

Write-Host "[stream-simulation] result=$resultJson"
Write-Host "[stream-simulation] overall_passed=$overallPassed scenarios=$($scenarios.Count)"

if (-not $overallPassed) {
    exit 1
}

exit 0
