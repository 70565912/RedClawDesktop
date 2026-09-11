param(
    [string]$HostServiceBinaryPath = "",
    [string]$OutputJsonPath = "",
    [string]$SessionId = "m07-uac-e2e-session",
    [string]$OperatorId = "",
    [string]$DeviceId = "",
    [ValidateSet("pass", "fail", "skip")]
    [string]$ScenarioAResult = "",
    [ValidateSet("pass", "fail", "skip")]
    [string]$ScenarioBResult = "",
    [ValidateSet("pass", "fail", "skip")]
    [string]$ScenarioCResult = "",
    [string]$ScenarioANote = "",
    [string]$ScenarioBNote = "",
    [string]$ScenarioCNote = "",
    [ValidateSet("scenario_a_authorized", "scenario_b_deny_timeout", "scenario_c_unauthorized_block")]
    [string[]]$AllowSkipScenarios = @(),
    [string]$SkipReason = "dual-machine-step-not-applicable",
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "This script requires Administrator privileges. Start PowerShell as Administrator and retry."
    }
}

function Resolve-DefaultPath {
    param(
        [string]$Value,
        [Parameter(Mandatory = $true)]
        [string]$Default
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $Default
    }

    if ([System.IO.Path]::IsPathRooted($Value)) {
        return $Value
    }

    return Join-Path $repoRoot $Value
}

function Read-ScenarioResult {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScenarioName,
        [string]$Provided,
        [switch]$NonInteractiveMode
    )

    if (-not [string]::IsNullOrWhiteSpace($Provided)) {
        return $Provided
    }

    if ($NonInteractiveMode) {
        return "skip"
    }

    while ($true) {
        $answer = Read-Host "$ScenarioName result [pass/fail/skip]"
        $normalized = $answer.Trim().ToLowerInvariant()
        if ($normalized -in @("pass", "fail", "skip")) {
            return $normalized
        }

        Write-Host "Invalid input. Please enter pass, fail, or skip." -ForegroundColor Yellow
    }
}

function Is-ScenarioAccepted {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScenarioName,
        [Parameter(Mandatory = $true)]
        [string]$ResultValue,
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.HashSet[string]]$AllowedSkipSet
    )

    if ($ResultValue -eq "pass") {
        return $true
    }

    if ($ResultValue -eq "skip" -and $AllowedSkipSet.Contains($ScenarioName)) {
        return $true
    }

    return $false
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptRoot)
$reportRoot = Join-Path $repoRoot "build/reports"

$defaultBinary = Join-Path $repoRoot "build/vs2022-x64/src/Debug/redclaw_desktop.exe"
$resolvedBinaryPath = Resolve-DefaultPath -Value $HostServiceBinaryPath -Default $defaultBinary

$defaultOutputJsonPath = Join-Path $reportRoot "m07-t04-uac-e2e-validation.json"
$resolvedOutputJsonPath = Resolve-DefaultPath -Value $OutputJsonPath -Default $defaultOutputJsonPath

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedOutputJsonPath) | Out-Null

$result = [ordered]@{
    passed = $false
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    mode = if ($NonInteractive) { "non-interactive" } else { "interactive" }
    session_id = $SessionId
    operator_id = $OperatorId
    device_id = $DeviceId
    host_service_binary_path = $resolvedBinaryPath
    checks = [ordered]@{
        admin = $false
        host_binary_exists = $false
        scenario_a_authorized = "skip"
        scenario_b_deny_timeout = "skip"
        scenario_c_unauthorized_block = "skip"
    }
    notes = [ordered]@{
        scenario_a = $ScenarioANote
        scenario_b = $ScenarioBNote
        scenario_c = $ScenarioCNote
    }
    evidence = [ordered]@{
        expected_audit_fields = @(
            "operator_id",
            "session_id",
            "decision",
            "error",
            "timestamp_unix"
        )
        checklist_doc = "docs/testing/e2e-privileged-directx-checklist.md"
        runbook_doc = "docs/testing/m07-t04-uac-e2e-runbook.md"
    }
    execution_policy = [ordered]@{
        allow_skip_scenarios = $AllowSkipScenarios
        skip_reason = $SkipReason
        accepted_skips = @()
    }
    error = ""
}

try {
    Assert-Administrator
    $result.checks.admin = $true

    if (-not (Test-Path $resolvedBinaryPath)) {
        throw "Host service binary not found: $resolvedBinaryPath"
    }
    $result.checks.host_binary_exists = $true

    if (-not $NonInteractive) {
        Write-Host "Scenario A (authorized UAC approve): run the remote flow and decide pass/fail." -ForegroundColor Cyan
        Write-Host "Scenario B (authorized UAC deny/timeout): run deny/timeout and decide pass/fail." -ForegroundColor Cyan
        Write-Host "Scenario C (unauthorized UAC block): run unauthorized path and decide pass/fail." -ForegroundColor Cyan
    }

    $result.checks.scenario_a_authorized = Read-ScenarioResult -ScenarioName "Scenario A" -Provided $ScenarioAResult -NonInteractiveMode:$NonInteractive
    $result.checks.scenario_b_deny_timeout = Read-ScenarioResult -ScenarioName "Scenario B" -Provided $ScenarioBResult -NonInteractiveMode:$NonInteractive
    $result.checks.scenario_c_unauthorized_block = Read-ScenarioResult -ScenarioName "Scenario C" -Provided $ScenarioCResult -NonInteractiveMode:$NonInteractive

    $allowedSkipSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($scenarioName in $AllowSkipScenarios) {
        [void]$allowedSkipSet.Add($scenarioName)
    }

    $scenarioStatusMap = [ordered]@{
        scenario_a_authorized = [string]$result.checks.scenario_a_authorized
        scenario_b_deny_timeout = [string]$result.checks.scenario_b_deny_timeout
        scenario_c_unauthorized_block = [string]$result.checks.scenario_c_unauthorized_block
    }

    foreach ($entry in $scenarioStatusMap.GetEnumerator()) {
        if ($entry.Value -eq "skip" -and $allowedSkipSet.Contains($entry.Key)) {
            $result.execution_policy.accepted_skips += [ordered]@{
                scenario = $entry.Key
                reason = $SkipReason
            }
        }
    }

    $scenarioAOk = Is-ScenarioAccepted -ScenarioName "scenario_a_authorized" -ResultValue ([string]$result.checks.scenario_a_authorized) -AllowedSkipSet $allowedSkipSet
    $scenarioBOk = Is-ScenarioAccepted -ScenarioName "scenario_b_deny_timeout" -ResultValue ([string]$result.checks.scenario_b_deny_timeout) -AllowedSkipSet $allowedSkipSet
    $scenarioCOk = Is-ScenarioAccepted -ScenarioName "scenario_c_unauthorized_block" -ResultValue ([string]$result.checks.scenario_c_unauthorized_block) -AllowedSkipSet $allowedSkipSet

    foreach ($entry in $scenarioStatusMap.GetEnumerator()) {
        if ($entry.Value -eq "skip" -and -not $allowedSkipSet.Contains($entry.Key)) {
            throw "Scenario '$($entry.Key)' is marked as skip, but it is not listed in -AllowSkipScenarios."
        }
    }

    $result.passed = $result.checks.admin -and $result.checks.host_binary_exists -and $scenarioAOk -and $scenarioBOk -and $scenarioCOk
}
catch {
    $result.error = $_.Exception.Message
}
finally {
    $result.completed_utc = (Get-Date).ToUniversalTime().ToString("o")
    $result | ConvertTo-Json -Depth 8 | Out-File -FilePath $resolvedOutputJsonPath -Encoding utf8

    Write-Host "Validation result file: $resolvedOutputJsonPath"
    if ($result.passed) {
        Write-Host "Result: PASS"
        exit 0
    }

    if (-not [string]::IsNullOrWhiteSpace($result.error)) {
        Write-Host "Result: FAIL"
        Write-Host "Error: $($result.error)"
        exit 1
    }

    Write-Host "Result: INCOMPLETE_OR_FAIL (check scenario statuses in JSON report)"
    exit 1
}
