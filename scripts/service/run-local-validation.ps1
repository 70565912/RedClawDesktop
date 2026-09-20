[CmdletBinding()]
param(
    [ValidateSet('smoke', 'input', 'workspace', 'agent', 'media', 'connection', 'ui')]
    [string[]]$Area = @('smoke'),
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$BuildDirectory = '',
    [string]$ReportRoot = 'build/reports',
    [switch]$List,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

function Get-LocalValidationPlan {
    param([string[]]$Areas)

    # Test names are also build targets. Reuse production CTests, not another harness.
    $groups = [ordered]@{
        smoke = @('redclaw_protocol_stream_control_protocol_tests',
            'redclaw_remote_input_session_tests', 'redclaw_transfer_operation_gate_tests',
            'redclaw_terminal_protocol_tests', 'redclaw_agent_remote_agent_broker_tests')
        input = @('redclaw_protocol_stream_control_protocol_tests',
            'redclaw_remote_input_session_tests', 'redclaw_input_policy_gate_tests',
            'redclaw_input_injection_adapter_tests', 'redclaw_clipboard_paste_guard_tests')
        workspace = @('redclaw_transfer_protocol_tests', 'redclaw_terminal_protocol_tests',
            'redclaw_transfer_file_receiver_tests', 'redclaw_transfer_operation_gate_tests',
            'redclaw_transfer_worker_integration_tests', 'redclaw_transfer_runtime_integration_tests',
            'redclaw_clipboard_paste_guard_tests', 'redclaw_clipboard_payload_integration_tests',
            'redclaw_clipboard_copy_store_integration_tests', 'redclaw_terminal_session_integration_tests')
        agent = @('redclaw_protocol_agent_protocol_tests', 'redclaw_agent_peer_session_tests',
            'redclaw_agent_remote_agent_broker_tests', 'redclaw_agent_coordination_tests',
            'redclaw_agent_provider_tests')
        media = @('redclaw_capture_recovery_cursor_tests', 'redclaw_stream_adaptation_regression_tests',
            'redclaw_net_video_frame_transport_tests', 'redclaw_ffmpeg_roundtrip_integration_tests',
            'redclaw_desktop_stream_simulation_integration_tests')
        connection = @('redclaw_service_dht_rendezvous_tests',
            'redclaw_service_connection_negotiation_tests', 'redclaw_transport_recovery_regression_tests')
        ui = @('redclaw_ui_connection_flow_tests', 'redclaw_ui_agent_control_server_tests',
            'redclaw_ui_debug_control_protocol_tests', 'redclaw_terminal_view_integration_tests')
    }
    if ($Areas.Count -eq 0) { throw 'Select at least one validation area.' }
    $targets = @(@(foreach ($item in $Areas) {
        if (-not $groups.Contains($item)) { throw "Unknown validation area: $item" }
        $groups[$item]
    }) | Sort-Object -Unique)
    return [pscustomobject]@{
        areas = @($Areas | Sort-Object -Unique)
        targets = $targets
        regex = '^(' + (($targets | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
    }
}

function Get-LocalValidationOutcome {
    param([xml]$JUnit, [string[]]$Expected, [int]$ExitCode, [int]$CaseSkips = 0)

    $rows = @($JUnit.SelectNodes('//testcase'))
    $names = @($rows | ForEach-Object { $_.GetAttribute('name') })
    $missing = @($Expected | Where-Object { $_ -notin $names })
    $unexpected = @($names | Where-Object { $_ -notin $Expected })
    $failed = @($rows | Where-Object {
        $_.SelectNodes('failure|error').Count -gt 0 -or $_.GetAttribute('status') -eq 'fail'
    })
    $skipped = @($rows | Where-Object {
        $_.SelectNodes('skipped').Count -gt 0 -or $_.GetAttribute('status') -in @('notrun', 'disabled')
    })
    $status = 'passed'
    if ($ExitCode -ne 0 -or $Expected.Count -eq 0 -or $missing.Count -gt 0 -or
        $unexpected.Count -gt 0 -or ($names | Sort-Object -Unique).Count -ne $names.Count -or
        $failed.Count -gt 0) {
        $status = 'failed'
    } elseif ($skipped.Count -gt 0 -or $CaseSkips -gt 0) {
        # Manual cases are excluded at CTest registration, never queued as skips.
        # A skip in the automatic selection is an unexpected environment/test error.
        $status = 'failed'
    }
    return [pscustomobject]@{
        status = $status
        suites_total = $rows.Count
        suites_failed = $failed.Count
        suites_skipped = $skipped.Count
        case_skips_observed = $CaseSkips
        missing_tests = $missing
        unexpected_tests = $unexpected
    }
}

function Invoke-ValidationCommand {
    param([string]$Executable, [string[]]$Arguments, [string]$LogPath)
    # Keep native stderr in the evidence file (also works in Windows PowerShell 5.1).
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments > $LogPath 2>&1
        return $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
}

function Get-ValidationFileHash {
    param([string]$Path)
    # Independent of PSModulePath inherited from a PowerShell 7 parent into 5.1.
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}

$plan = Get-LocalValidationPlan -Areas $Area
if ($List) { $plan | ConvertTo-Json -Depth 4; exit 0 }

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not [IO.Path]::IsPathRooted($ReportRoot)) { $ReportRoot = Join-Path $repoRoot $ReportRoot }
$reportDirectory = Join-Path $ReportRoot ('local-validation-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$resultPath = Join-Path $reportDirectory 'result.json'
$result = [ordered]@{
    schema = 'redclaw.local-validation.v1'
    generated_at = (Get-Date).ToString('o')
    status = 'blocked'
    overall_passed = $false
    scope = 'local_selected_tests_only'
    configuration = $Configuration
    plan = $plan
    build_skipped = [bool]$SkipBuild
    build_exit_code = $null
    test_exit_code = $null
    source_commit = ''
    tracked_worktree_dirty = $null
    binaries = @()
    outcome = $null
    skipped_cases = @()
    case_report_count = 0
    error = ''
}
$exitCode = 3
$previousGtestOutput = $env:GTEST_OUTPUT
Push-Location $repoRoot
try {
    if (($env:GTEST_FILTER -and $env:GTEST_FILTER -ne '*') -or $env:GTEST_TOTAL_SHARDS -or
        $env:GTEST_SHARD_INDEX -or ($env:GTEST_REPEAT -and $env:GTEST_REPEAT -ne '1')) {
        throw 'Clear inherited GoogleTest filtering/sharding/repetition before suite validation; selected suites must run completely.'
    }
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
        $candidates = @(@('build/ninja-x64', 'build/vs2022-x64') | Where-Object {
            Test-Path -LiteralPath (Join-Path $_ 'CTestTestfile.cmake')
        })
        if ($candidates.Count -ne 1) {
            throw 'Specify -BuildDirectory: expected exactly one configured local CTest directory.'
        }
        $BuildDirectory = $candidates[0]
    }
    $BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt')
    $sourceEntry = @($cache | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' })
    if ($sourceEntry.Count -ne 1 -or
        [IO.Path]::GetFullPath(($sourceEntry[0] -split '=', 2)[1]).TrimEnd('\', '/') -ne $repoRoot.TrimEnd('\', '/')) {
        throw 'Selected build directory belongs to a different source tree; use this checkout''s configured build.'
    }
    $cmakeEntry = @($cache | Where-Object { $_ -like 'CMAKE_COMMAND:INTERNAL=*' })
    if ($cmakeEntry.Count -ne 1) { throw 'Configured CMake executable is missing from CMakeCache.txt.' }
    $cmake = $cmakeEntry[0].Substring($cmakeEntry[0].IndexOf('=') + 1)
    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctest)) { throw 'Configured CTest executable is unavailable.' }
    $result.build_directory = $BuildDirectory
    $result.source_commit = [string](& git rev-parse HEAD)
    $result.tracked_worktree_dirty = [bool](@(& git status --porcelain --untracked-files=no).Count)
    $inventoryPath = Join-Path $reportDirectory 'inventory.json'
    $code = Invoke-ValidationCommand $ctest @('--test-dir', $BuildDirectory, '-C', $Configuration,
        '--show-only=json-v1', '-R', $plan.regex) $inventoryPath
    if ($code -ne 0) { throw 'CTest discovery failed; see inventory.json.' }
    $inventory = Get-Content -LiteralPath $inventoryPath -Raw | ConvertFrom-Json
    $found = @($inventory.tests | ForEach-Object { $_.name })
    $missing = @($plan.targets | Where-Object { $_ -notin $found })
    if ($missing.Count -gt 0) { throw ('Selected tests are not configured: ' + ($missing -join ', ')) }

    if (-not $SkipBuild) {
        Write-Host "[local-validation] building $($plan.targets.Count) test targets (no app publish)"
        $buildArgs = @('--build', $BuildDirectory, '--config', $Configuration, '--target') + $plan.targets
        $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($buildArgs -join [char]31)))
        $result.build_exit_code = Invoke-ValidationCommand 'powershell.exe' @('-NoProfile',
            '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run-cmake-guarded.ps1'),
            '-CMakePath', $cmake, '-CMakeArgsBase64', $encoded,
            '-CommandTimeoutSeconds', '900') (Join-Path $reportDirectory 'build.log')
        if ($result.build_exit_code -ne 0) {
            $result.status = 'failed'; $exitCode = 1
            throw 'Focused build failed or was locked; see build.log. No automatic retry.'
        }
        # Refresh discovery: an executable missing before the build has no command in CTest JSON.
        $code = Invoke-ValidationCommand $ctest @('--test-dir', $BuildDirectory, '-C', $Configuration,
            '--show-only=json-v1', '-R', $plan.regex) $inventoryPath
        if ($code -ne 0) { throw 'Post-build CTest discovery failed.' }
        $inventory = Get-Content -LiteralPath $inventoryPath -Raw | ConvertFrom-Json
    }
    $result.binaries = @(foreach ($test in $inventory.tests) {
        if (-not $test.command -or -not (Test-Path -LiteralPath $test.command[0])) {
            throw "Test executable unavailable: $($test.name). Build selected targets first."
        }
        [ordered]@{ test = $test.name; sha256 = Get-ValidationFileHash $test.command[0];
            case_filter = @($test.command | Where-Object { $_ -like '--gtest_filter=*' }) }
    })
    $caseDirectory = Join-Path $reportDirectory 'cases'
    New-Item -ItemType Directory -Path $caseDirectory | Out-Null
    $env:GTEST_OUTPUT = 'xml:' + $caseDirectory.Replace('\', '/') + '/'
    $junitPath = Join-Path $reportDirectory 'ctest.xml'
    Write-Host "[local-validation] running $($plan.targets.Count) selected suites serially"
    $result.test_exit_code = Invoke-ValidationCommand $ctest @('--test-dir', $BuildDirectory,
        '-C', $Configuration, '-R', $plan.regex, '--parallel', '1', '--timeout', '120',
        '--no-tests=error', '--output-on-failure', '--output-junit', $junitPath) (Join-Path $reportDirectory 'ctest.log')
    $result.status = 'failed'; $exitCode = 1
    $caseReports = @(Get-ChildItem -LiteralPath $caseDirectory -Filter '*.xml' -File)
    $result.case_report_count = $caseReports.Count
    $emptyCaseReports = @()
    $result.skipped_cases = @(foreach ($file in $caseReports) {
        [xml]$cases = Get-Content -LiteralPath $file.FullName -Raw
        if ($cases.SelectNodes('//testcase').Count -eq 0) { $emptyCaseReports += $file.Name }
        foreach ($case in $cases.SelectNodes('//testcase[skipped or @status="notrun"]')) {
            [ordered]@{ suite = $case.GetAttribute('classname'); case = $case.GetAttribute('name');
                reason = $(if ($case.skipped) { $case.skipped.message } else { 'disabled' }) }
        }
    })
    [xml]$junit = Get-Content -LiteralPath $junitPath -Raw
    $result.outcome = Get-LocalValidationOutcome $junit $plan.targets $result.test_exit_code $result.skipped_cases.Count
    $result.status = $result.outcome.status
    if ($emptyCaseReports.Count -gt 0) {
        $result.status = 'failed'
        $result.error = 'Zero cases executed in: ' + ($emptyCaseReports -join ', ')
    }
    $result.overall_passed = $result.status -eq 'passed'
    $exitCode = if ($result.overall_passed) { 0 } else { 1 }
} catch {
    $result.error = $_.Exception.Message
} finally {
    $env:GTEST_OUTPUT = $previousGtestOutput
    Pop-Location
    $result.runner_exit_code = $exitCode
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    Write-Host "[local-validation] status=$($result.status) result=$resultPath"
    if ($result.error) { Write-Host "[local-validation] $($result.error)" }
}
exit $exitCode
