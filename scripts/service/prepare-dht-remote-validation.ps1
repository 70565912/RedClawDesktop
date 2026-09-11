param(
    [ValidateSet("host", "controller", "both")]
    [string]$Role = "both",

    [string]$SessionCode = "<HOST_CODE>",

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$ReportRoot = "build\reports",

    [ValidateRange(10, 3600)]
    [int]$RunSeconds = 240,

    [ValidateRange(0, 3600)]
    [int]$WaitSeconds = 300,

    [ValidateRange(0, 600)]
    [int]$StopAfterConnectedSeconds = 20,

    [bool]$EnableIceTcp = $true,

    [bool]$EnablePortMapping = $true,

    [switch]$DisableIpv6Candidates,

    [switch]$SkipBootstrapDoh,

    [switch]$SkipBootstrapReadiness,

    [switch]$SkipArchive,

    [switch]$SkipToolchainSelfTest,

    [switch]$NoCreateZip,

    [switch]$NoExistingReportRegression,

    [switch]$Json
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $candidate = Resolve-Path (Join-Path $PSScriptRoot "..\..")
    return $candidate.Path
}

function Get-RunId {
    return ("{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss_fff"), ([guid]::NewGuid().ToString("N").Substring(0, 6)))
}

function Get-RedactedSessionCode {
    param([string]$Code)

    if ([string]::IsNullOrWhiteSpace($Code) -or $Code -eq "<HOST_CODE>") {
        return "<HOST_CODE>"
    }
    if ($Code.Length -lt 4) {
        return "<redacted>"
    }

    return ($Code.Substring(0, 4) + "****")
}

function Test-SessionCodeShape {
    param([string]$Code)

    if ([string]::IsNullOrWhiteSpace($Code) -or $Code -eq "<HOST_CODE>") {
        return $true
    }

    return $Code -match '^[A-Za-z0-9]{8}$'
}

function New-CheckResult {
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Detail
    )

    [pscustomobject]@{
        name = $Name
        passed = $Passed
        detail = $Detail
    }
}

function Get-RuntimeFreshnessCheck {
    param(
        [string]$RuntimeExe,
        [string]$RepoRoot
    )

    if (-not (Test-Path $RuntimeExe)) {
        return New-CheckResult -Name "published_runtime_fresh" -Passed $false -Detail "runtime_missing"
    }

    $runtimeItem = Get-Item -Path $RuntimeExe
    $sourceFiles = @(
        "src/main.cpp",
        "src/service/src/dht_rendezvous.cpp",
        "src/service/src/dht_rendezvous_libtorrent.cpp",
        "src/service/include/redclaw/service/dht_rendezvous.h",
        "src/helper/src/runtime_profile.cpp",
        "src/helper/include/redclaw/helper/runtime_profile.h",
        "CMakeLists.txt",
        "src/service/CMakeLists.txt",
        "vcpkg.json"
    )

    $existingSources = @()
    foreach ($relativePath in $sourceFiles) {
        $sourcePath = Join-Path $RepoRoot $relativePath
        if (Test-Path $sourcePath) {
            $item = Get-Item -Path $sourcePath
            $existingSources += [pscustomobject]@{
                path = $relativePath
                last_write_time = $item.LastWriteTime
            }
        }
    }

    if ($existingSources.Count -eq 0) {
        return New-CheckResult -Name "published_runtime_fresh" -Passed $false -Detail "no_runtime_sources_found"
    }

    $newerSources = @(
        $existingSources |
            Where-Object { $_.last_write_time -gt $runtimeItem.LastWriteTime } |
            Sort-Object last_write_time -Descending
    )
    $newestSource = $existingSources | Sort-Object last_write_time -Descending | Select-Object -First 1
    $detail = "runtime={0:o}; newest_source={1} {2:o}" -f `
        $runtimeItem.LastWriteTime,
        $newestSource.path,
        $newestSource.last_write_time

    if ($newerSources.Count -gt 0) {
        $newerDetail = ($newerSources | Select-Object -First 3 | ForEach-Object {
                "{0} {1:o}" -f $_.path, $_.last_write_time
            }) -join "; "
        return New-CheckResult -Name "published_runtime_fresh" -Passed $false -Detail ("newer_source_than_runtime: {0}" -f $newerDetail)
    }

    return New-CheckResult -Name "published_runtime_fresh" -Passed $true -Detail $detail
}

function ConvertTo-CommandLine {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    $parts = @($Command)
    foreach ($argument in $Arguments) {
        if ($argument -match '[\s"]') {
            $parts += ('"{0}"' -f ($argument -replace '"', '\"'))
        } else {
            $parts += $argument
        }
    }

    return ($parts -join " ")
}

function New-WorkflowCommand {
    param([string]$CommandRole)

    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "scripts/service/run-dht-role-validation-workflow.ps1",
        "-Role",
        $CommandRole,
        "-SessionCode",
        "<HOST_CODE>",
        "-Configuration",
        $Configuration,
        "-RunSeconds",
        ([string]$RunSeconds),
        "-WaitSeconds",
        ([string]$WaitSeconds),
        "-StopAfterConnectedSeconds",
        ([string]$StopAfterConnectedSeconds)
    )

    if ($EnableIceTcp) {
        $arguments += "-EnableIceTcp"
    }
    if ($EnablePortMapping) {
        $arguments += "-EnablePortMapping"
    }
    if ($DisableIpv6Candidates) {
        $arguments += "-DisableIpv6Candidates"
    }
    if ($SkipBootstrapDoh) {
        $arguments += "-SkipBootstrapDoh"
    }
    if ($SkipBootstrapReadiness) {
        $arguments += "-SkipBootstrapReadiness"
    }
    if ($SkipArchive) {
        $arguments += "-SkipArchive"
    }
    if ($SkipToolchainSelfTest) {
        $arguments += "-SkipToolchainSelfTest"
    }
    if (-not $NoCreateZip) {
        $arguments += "-CreateZip"
    }
    if (-not $NoExistingReportRegression) {
        $arguments += "-IncludeExistingReportRegression"
    }

    return ConvertTo-CommandLine -Command "powershell.exe" -Arguments $arguments
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$workflowScript = Join-Path $PSScriptRoot "run-dht-role-validation-workflow.ps1"
$workflowSummaryScript = Join-Path $PSScriptRoot "summarize-dht-workflow-result.ps1"
$bootstrapScript = Join-Path $PSScriptRoot "test-dht-bootstrap-readiness.ps1"
$runtimeExe = Join-Path $repoRoot ("release\{0}\redclaw_desktop.exe" -f $Configuration)

$checks = @()
$checks += New-CheckResult -Name "workflow_script" -Passed (Test-Path $workflowScript) -Detail $workflowScript
$checks += New-CheckResult -Name "workflow_summary_script" -Passed (Test-Path $workflowSummaryScript) -Detail $workflowSummaryScript
$checks += New-CheckResult -Name "bootstrap_preflight_script" -Passed (Test-Path $bootstrapScript) -Detail $bootstrapScript
$checks += New-CheckResult -Name "published_runtime" -Passed (Test-Path $runtimeExe) -Detail $runtimeExe
$checks += Get-RuntimeFreshnessCheck -RuntimeExe $runtimeExe -RepoRoot $repoRoot
$checks += New-CheckResult -Name "session_code_shape" -Passed (Test-SessionCodeShape -Code $SessionCode) -Detail (Get-RedactedSessionCode -Code $SessionCode)

$roles = if ($Role -eq "both") { @("host", "controller") } else { @($Role) }
$commands = @()
foreach ($commandRole in $roles) {
    $commands += [pscustomobject]@{
        role = $commandRole
        command = New-WorkflowCommand -CommandRole $commandRole
    }
}

$failedChecks = @($checks | Where-Object { -not $_.passed })
$runId = Get-RunId
$reportRootAbsolute = Join-Path $repoRoot $ReportRoot
$outputPath = Join-Path $reportRootAbsolute ("dht-remote-validation-prep-{0}.json" -f $runId)

$result = [pscustomobject]@{
    schema = "redclaw.dht.remote.validation.prep.v1"
    generated_at = (Get-Date).ToString("o")
    status = if ($failedChecks.Count -eq 0) { "ready" } else { "not_ready" }
    role = $Role
    session_code_redacted = Get-RedactedSessionCode -Code $SessionCode
    configuration = $Configuration
    runtime_exe = $runtimeExe
    report = $outputPath
    checks = @($checks)
    commands = @($commands)
    expected_outputs = [pscustomobject]@{
        workflow_result = "build/reports/dht-<role>-workflow-result-<run-id>.json"
        workflow_result_summary = "build/reports/dht-<role>-workflow-result-summary-<run-id>.json"
        evidence_zip = "build/reports/dht-<role>-workflow-evidence-<run-id>.zip"
    }
    notes = @(
        "Commands intentionally keep <HOST_CODE> as a placeholder; paste the active 8-character code at execution time.",
        "A warning_suspicious_or_partial_bootstrap result can still be usable when runtime cached fallback endpoints exist.",
        "CLI workflow evidence does not replace the required cross-LAN Controller UI playback proof."
    )
}

New-Item -ItemType Directory -Force -Path $reportRootAbsolute | Out-Null
$result | ConvertTo-Json -Depth 8 | Set-Content -Path $outputPath -Encoding UTF8

if ($Json) {
    $result | ConvertTo-Json -Depth 8
} else {
    Write-Host ("[dht-prep] status: {0}" -f $result.status)
    Write-Host ("[dht-prep] report: {0}" -f $outputPath)
    foreach ($check in $checks) {
        Write-Host ("[dht-prep] check {0}: {1} ({2})" -f $check.name, $(if ($check.passed) { "pass" } else { "fail" }), $check.detail)
    }
    foreach ($command in $commands) {
        Write-Host ("[dht-prep] {0} command:" -f $command.role)
        Write-Host $command.command
    }
    Write-Host "[dht-prep] Replace <HOST_CODE> only at execution time; this prep report does not store the full code."
}

if ($failedChecks.Count -gt 0) {
    exit 1
}

exit 0
