param(
    [Parameter(Mandatory = $true)]
    [string]$ScenarioAAuditPath,

    [Parameter(Mandatory = $true)]
    [string]$ScenarioBAuditPath,

    [Parameter(Mandatory = $true)]
    [string]$ScenarioCAuditPath,

    [string]$OutputJsonPath = "build/reports/m07-t04-uac-audit-evidence-validation.json",

    [string]$ExpectedSessionId = "",

    [string]$ExpectedOperatorId = ""
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptRoot)

function Resolve-RepoPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }

    return Join-Path $repoRoot $PathValue
}

function Load-AuditEvents {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    $resolvedPath = Resolve-RepoPath -PathValue $PathValue
    if (-not (Test-Path $resolvedPath)) {
        throw "Audit evidence file not found: $resolvedPath"
    }

    $raw = Get-Content -Raw -Path $resolvedPath
    if ([string]::IsNullOrWhiteSpace($raw)) {
        throw "Audit evidence file is empty: $resolvedPath"
    }

    $parsed = $raw | ConvertFrom-Json

    if ($null -eq $parsed) {
        throw "Failed to parse audit evidence JSON: $resolvedPath"
    }

    if ($parsed -is [System.Array]) {
        return ,$parsed
    }

    if ($parsed.PSObject.Properties.Name -contains "audits") {
        return ,$parsed.audits
    }

    if ($parsed.PSObject.Properties.Name -contains "action") {
        return ,@($parsed)
    }

    throw "Unsupported audit evidence JSON shape: $resolvedPath"
}

function Get-EventStringValue {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Event,
        [Parameter(Mandatory = $true)]
        [string]$Field
    )

    $prop = $Event.PSObject.Properties[$Field]
    if ($null -eq $prop) {
        return $null
    }

    if ($null -eq $prop.Value) {
        return ""
    }

    return [string]$prop.Value
}

function Get-ScenarioValidation {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScenarioName,
        [Parameter(Mandatory = $true)]
        [object[]]$Events,
        [string]$ExpectedSession,
        [string]$ExpectedOperator
    )

    $result = [ordered]@{
        scenario = $ScenarioName
        passed = $false
        event_count = $Events.Count
        checks = [ordered]@{
            required_fields_present = $true
            session_id_match = $true
            operator_id_present = $true
            decision_mapping = $false
        }
        details = @()
    }

    if ($Events.Count -eq 0) {
        $result.checks.required_fields_present = $false
        $result.details += "No events found"
        return $result
    }

    foreach ($event in $Events) {
        foreach ($field in @("session_id", "operator_id", "decision", "error", "timestamp_unix")) {
            $value = Get-EventStringValue -Event $event -Field $field
            if ($null -eq $value) {
                $result.checks.required_fields_present = $false
                $result.details += "Missing required field '$field'"
                break
            }

            if ($field -ne "operator_id" -and [string]::IsNullOrWhiteSpace($value)) {
                $result.checks.required_fields_present = $false
                $result.details += "Missing or empty required field '$field'"
                break
            }
        }

        if (-not $result.checks.required_fields_present) {
            break
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedSession)) {
        $allSessionMatch = $true
        foreach ($event in $Events) {
            $sessionId = Get-EventStringValue -Event $event -Field "session_id"
            if ($sessionId -ne $ExpectedSession) {
                $allSessionMatch = $false
                break
            }
        }

        $result.checks.session_id_match = $allSessionMatch
        if (-not $allSessionMatch) {
            $result.details += "Found event with mismatched session_id"
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedOperator)) {
        $hasExpectedOperator = $false
        foreach ($event in $Events) {
            $operatorId = Get-EventStringValue -Event $event -Field "operator_id"
            if ($operatorId -eq $ExpectedOperator) {
                $hasExpectedOperator = $true
                break
            }
        }

        $result.checks.operator_id_present = $hasExpectedOperator
        if (-not $hasExpectedOperator) {
            $result.details += "Expected operator_id '$ExpectedOperator' not present"
        }
    }

    $hasAllowApplied = $false
    $hasDenyApplied = $false
    $hasTimeoutFailClose = $false
    $hasUnauthorizedBlock = $false

    foreach ($event in $Events) {
        $action = Get-EventStringValue -Event $event -Field "action"
        $detail = Get-EventStringValue -Event $event -Field "detail"
        $decision = Get-EventStringValue -Event $event -Field "decision"
        $error = Get-EventStringValue -Event $event -Field "error"

        if ($action -eq "confirm_uac" -and $detail -eq "applied" -and $decision -eq "allow" -and $error -eq "none") {
            $hasAllowApplied = $true
        }

        if ($action -eq "confirm_uac" -and $detail -eq "applied" -and $decision -eq "deny" -and $error -eq "none") {
            $hasDenyApplied = $true
        }

        if ($action -eq "uac_prompt_state" -and $decision -eq "blocked" -and $error -eq "secure_desktop_unavailable") {
            if ($detail -eq "timeout" -or $detail -eq "closed" -or $detail -eq "secure_desktop_unavailable" -or $detail -eq "secure_desktop_transport_lost") {
                $hasTimeoutFailClose = $true
            }
        }

        if ($decision -eq "blocked") {
            if ($error -in @("policy_denied", "step_up_required", "replay_detected", "rate_limited", "invalid_session", "token_expired", "secure_desktop_unavailable")) {
                $hasUnauthorizedBlock = $true
            }
        }
    }

    switch ($ScenarioName) {
        "scenario_a_authorized" {
            $result.checks.decision_mapping = $hasAllowApplied
            if (-not $hasAllowApplied) {
                $result.details += "Expected allow-applied confirm_uac event not found"
            }
        }
        "scenario_b_deny_timeout" {
            $result.checks.decision_mapping = ($hasDenyApplied -or $hasTimeoutFailClose)
            if (-not $result.checks.decision_mapping) {
                $result.details += "Expected deny-applied or timeout/closed fail-close evidence not found"
            }
        }
        "scenario_c_unauthorized_block" {
            $result.checks.decision_mapping = $hasUnauthorizedBlock
            if (-not $hasUnauthorizedBlock) {
                $result.details += "Expected blocked decision with policy/security denial reason not found"
            }
        }
        default {
            $result.checks.decision_mapping = $false
            $result.details += "Unsupported scenario name"
        }
    }

    $result.passed = $result.checks.required_fields_present `
        -and $result.checks.session_id_match `
        -and $result.checks.operator_id_present `
        -and $result.checks.decision_mapping

    if ($result.passed -and $result.details.Count -eq 0) {
        $result.details += "Scenario evidence checks passed"
    }

    return $result
}

$scenarioAPath = Resolve-RepoPath -PathValue $ScenarioAAuditPath
$scenarioBPath = Resolve-RepoPath -PathValue $ScenarioBAuditPath
$scenarioCPath = Resolve-RepoPath -PathValue $ScenarioCAuditPath
$outputPath = Resolve-RepoPath -PathValue $OutputJsonPath

$report = [ordered]@{
    passed = $false
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    expected_session_id = $ExpectedSessionId
    expected_operator_id = $ExpectedOperatorId
    inputs = [ordered]@{
        scenario_a = $scenarioAPath
        scenario_b = $scenarioBPath
        scenario_c = $scenarioCPath
    }
    scenarios = @()
    summary = [ordered]@{
        total = 3
        passed = 0
        failed = 0
    }
    error = ""
}

try {
    $eventsA = Load-AuditEvents -PathValue $ScenarioAAuditPath
    $eventsB = Load-AuditEvents -PathValue $ScenarioBAuditPath
    $eventsC = Load-AuditEvents -PathValue $ScenarioCAuditPath

    $scenarioAResult = Get-ScenarioValidation -ScenarioName "scenario_a_authorized" -Events $eventsA -ExpectedSession $ExpectedSessionId -ExpectedOperator $ExpectedOperatorId
    $scenarioBResult = Get-ScenarioValidation -ScenarioName "scenario_b_deny_timeout" -Events $eventsB -ExpectedSession $ExpectedSessionId -ExpectedOperator $ExpectedOperatorId
    $scenarioCResult = Get-ScenarioValidation -ScenarioName "scenario_c_unauthorized_block" -Events $eventsC -ExpectedSession $ExpectedSessionId -ExpectedOperator $ExpectedOperatorId

    $report.scenarios += $scenarioAResult
    $report.scenarios += $scenarioBResult
    $report.scenarios += $scenarioCResult

    foreach ($scenario in $report.scenarios) {
        if ($scenario.passed) {
            $report.summary.passed += 1
        }
        else {
            $report.summary.failed += 1
        }
    }

    $report.passed = ($report.summary.failed -eq 0)
}
catch {
    $report.error = $_.Exception.Message
}
finally {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outputPath) | Out-Null
    $report.completed_utc = (Get-Date).ToUniversalTime().ToString("o")
    $report | ConvertTo-Json -Depth 8 | Out-File -FilePath $outputPath -Encoding utf8

    Write-Host "Audit validation report: $outputPath"
    if ($report.passed) {
        Write-Host "Result: PASS"
        exit 0
    }

    if (-not [string]::IsNullOrWhiteSpace($report.error)) {
        Write-Host "Result: FAIL"
        Write-Host "Error: $($report.error)"
        exit 1
    }

    Write-Host "Result: FAIL (check scenario details in report JSON)"
    exit 1
}
