$ErrorActionPreference = 'Stop'

# Load the actual pure helpers, without building/running product tests.
$runner = Join-Path $PSScriptRoot 'run-local-validation.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($runner, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Validation runner has syntax errors.' }
foreach ($name in @('Get-LocalValidationPlan', 'Get-LocalValidationOutcome')) {
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false)
    if ($null -eq $definition) { throw "Missing production helper: $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}

function Assert-Validation {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$plan = Get-LocalValidationPlan @('input', 'workspace', 'input')
Assert-Validation ($plan.areas.Count -eq 2) 'Duplicate area was not removed.'
Assert-Validation ($plan.targets.Count -eq ($plan.targets | Sort-Object -Unique).Count) 'Duplicate test selected.'
Assert-Validation ('redclaw_clipboard_paste_guard_tests' -in $plan.targets) 'Shared gate missing.'
Assert-Validation ('redclaw_clipboard_paste_guard_tests_extra' -notmatch $plan.regex) 'Regex is not exact.'
Assert-Validation ('redclaw_desktop' -notin $plan.targets) 'Runner must not build/publish the main program.'
$rejected = $false
try { $null = Get-LocalValidationPlan @('unknown') } catch { $rejected = $true }
Assert-Validation $rejected 'Unknown area silently passed.'

# Every catalog entry must still be a registered CTest and executable target.
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$cmakeFiles = @(Get-Item (Join-Path $repoRoot 'tests/CMakeLists.txt')) +
    @(Get-ChildItem (Join-Path $repoRoot 'tests/cmake') -Filter '*.cmake' -File)
$cmakeText = ($cmakeFiles | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
$all = Get-LocalValidationPlan @('smoke', 'input', 'workspace', 'agent', 'media', 'connection', 'ui')
foreach ($target in $all.targets) {
    $escaped = [regex]::Escape($target)
    Assert-Validation ($cmakeText -match "add_test\(\s*NAME\s+$escaped\s") "Not registered: $target"
    Assert-Validation ($cmakeText -match "add_executable\(\s*$escaped\s") "Not a build target: $target"
}

$manualFilters = @{
    redclaw_clipboard_payload_integration_tests = @('ClipboardNativeFocus.*', 'ClipboardPayloadFixture.*')
    redclaw_agent_provider_tests = @('AgentProviders.RealProviderReadinessWhenExplicitlyEnabled')
    redclaw_ui_connection_flow_tests = @('RuntimeLogView.DiagnosticPaintReplay',
        'ControllerRemoteInputCapture.ClipboardCallbackWithholdsPasteAndRepeatUntilKeyUp')
    redclaw_terminal_view_integration_tests = @('TerminalBridgeIntegration.RealKeyboardThroughPanelPipeAndWirePreservesShellOnReconnect',
        'TerminalViewIntegration.BundledSurfaceParsesRealShellAndSurvivesCollapse')
    redclaw_capture_recovery_cursor_tests = @('CaptureHardwareRecovery.*', 'CaptureCursor.D3D11MatchesCpuForShapesClippingAndRotation')
    redclaw_ffmpeg_roundtrip_integration_tests = @('FfmpegRoundtripIntegration.EncodesAndDecodesSingleFrame')
}
foreach ($target in $manualFilters.Keys) {
    $registration = [regex]::Match($cmakeText, ('(?s)add_test\(\s*NAME\s+' + [regex]::Escape($target) + '\s+[^)]*\)')).Value
    foreach ($case in $manualFilters[$target]) {
        Assert-Validation ($registration.Contains($case) -and $registration.Contains('--gtest_filter=-')) "Manual case leaked into CTest: $case"
    }
}
foreach ($target in @('redclaw_capture_dda_min_capture_poc_tests',
    'redclaw_capture_windows_capture_abstraction_skeleton_tests', 'redclaw_capture_stability_smoke_tests')) {
    Assert-Validation ($cmakeText -notmatch ('add_test\(\s*NAME\s+' + [regex]::Escape($target) + '\s')) "Manual probe registered in CTest: $target"
}

$expected = @('one', 'two')
[xml]$ok = '<testsuite><testcase name="one" status="run"/><testcase name="two" status="run"/></testsuite>'
Assert-Validation ((Get-LocalValidationOutcome $ok $expected 0).status -eq 'passed') 'Normal run failed.'
Assert-Validation ((Get-LocalValidationOutcome $ok $expected 1).status -eq 'failed') 'Nonzero exit passed.'
Assert-Validation ((Get-LocalValidationOutcome $ok $expected 0 1).status -eq 'failed') 'Unexpected GoogleTest skip hidden.'
[xml]$empty = '<testsuite/>'
Assert-Validation ((Get-LocalValidationOutcome $empty $expected 0).status -eq 'failed') 'Zero tests passed.'
Assert-Validation ((Get-LocalValidationOutcome $ok @('one', 'two', 'missing') 0).status -eq 'failed') 'Missing test passed.'
[xml]$failure = '<testsuite><testcase name="one"><failure/></testcase><testcase name="two"/></testsuite>'
Assert-Validation ((Get-LocalValidationOutcome $failure $expected 0).status -eq 'failed') 'Failure node ignored.'
[xml]$skip = '<testsuite><testcase name="one"><skipped/></testcase><testcase name="two"/></testsuite>'
Assert-Validation ((Get-LocalValidationOutcome $skip $expected 0).status -eq 'failed') 'Unexpected CTest skip hidden.'
[xml]$duplicate = '<testsuite><testcase name="one"/><testcase name="one"/></testsuite>'
Assert-Validation ((Get-LocalValidationOutcome $duplicate @('one') 0).status -eq 'failed') 'Duplicate test passed.'
Assert-Validation ((Get-LocalValidationOutcome $ok @('one') 0).status -eq 'failed') 'Unexpected test passed.'

# Exercise the public CLI and real JSON/exit-code contract, with no build or test process.
$reportRoot = Join-Path $repoRoot ('build/reports/local-validation-selftest-' + [guid]::NewGuid().ToString('N'))
$shell = (Get-Process -Id $PID).Path
$oldFilter = $env:GTEST_FILTER
$oldShards = $env:GTEST_TOTAL_SHARDS
$oldIndex = $env:GTEST_SHARD_INDEX
$oldRepeat = $env:GTEST_REPEAT
try {
    $env:GTEST_FILTER = $null; $env:GTEST_TOTAL_SHARDS = $null
    $env:GTEST_SHARD_INDEX = $null; $env:GTEST_REPEAT = $null
    $listed = (& $shell -NoProfile -File $runner -Area smoke -List | Out-String) | ConvertFrom-Json
    Assert-Validation ($LASTEXITCODE -eq 0 -and $listed.targets.Count -eq 5) 'List CLI contract failed.'
    $null = & $shell -NoProfile -File $runner -BuildDirectory (Join-Path $reportRoot 'missing') -ReportRoot $reportRoot -SkipBuild
    Assert-Validation ($LASTEXITCODE -eq 3) 'Missing build must return blocked/3.'
    $env:GTEST_FILTER = 'NoSuchSuite.*'
    $null = & $shell -NoProfile -File $runner -ReportRoot $reportRoot -SkipBuild
    Assert-Validation ($LASTEXITCODE -eq 3) 'Inherited case filter must not produce a false pass.'
    $reports = @(Get-ChildItem -LiteralPath $reportRoot -Filter result.json -Recurse -File)
    Assert-Validation ($reports.Count -eq 2) 'Blocked runs did not retain independent receipts.'
    foreach ($report in $reports) {
        $receipt = Get-Content -LiteralPath $report.FullName -Raw | ConvertFrom-Json
        Assert-Validation ($receipt.status -eq 'blocked' -and -not $receipt.overall_passed) 'Invalid blocked receipt.'
        Assert-Validation ($null -eq $receipt.build_exit_code -and $null -eq $receipt.test_exit_code) 'Blocked run executed work.'
    }
} finally {
    $env:GTEST_FILTER = $oldFilter; $env:GTEST_TOTAL_SHARDS = $oldShards
    $env:GTEST_SHARD_INDEX = $oldIndex; $env:GTEST_REPEAT = $oldRepeat
}

Write-Host "Passed: selection/deduplication, $($all.targets.Count) registered targets, manual-case exclusion, result/exit/skip/missing/duplicate checks, CLI blocked receipts."
