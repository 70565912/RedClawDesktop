param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("host", "controller")]
    [string]$Role,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9]{8}$')]
    [string]$SessionCode,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$RuntimeExe = "",

    [string]$ReportRoot = "build\reports",

    [string[]]$IceServer = @("stun:stun.l.google.com:19302"),

    [string[]]$DhtBootstrap = @(
        "router.bittorrent.com:6881",
        "dht.transmissionbt.com:6881",
        "dht.libtorrent.org:25401",
        "router.utorrent.com:6881"
    ),

    [ValidateRange(0, 65535)]
    [int]$DhtListenPort = 0,

    [ValidateRange(250, 60000)]
    [int]$DhtPollIntervalMs = 1000,

    [ValidateRange(1000, 120000)]
    [int]$DhtPublishRetryMs = 1000,

    [ValidateRange(10, 3600)]
    [int]$RunSeconds = 240,

    [ValidateRange(0, 3600)]
    [int]$WaitSeconds = 0,

    [ValidateRange(0, 600)]
    [int]$StopAfterConnectedSeconds = 20,

    [ValidateRange(500, 30000)]
    [int]$BootstrapDnsTimeoutMs = 5000,

    [bool]$EnableIceTcp = $true,

    [bool]$EnablePortMapping = $true,

    [switch]$DisableIpv6Candidates,

    [switch]$SkipBootstrapDoh,

    [switch]$SkipBootstrapReadiness,

    [switch]$SkipArchive,

    [switch]$SkipToolchainSelfTest,

    [switch]$IncludeExistingReportRegression,

    [switch]$CreateZip,

    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $candidate = Resolve-Path (Join-Path $PSScriptRoot "..\..")
    return $candidate.Path
}

function Get-RedactedSessionCode {
    param([string]$Code)

    if ([string]::IsNullOrWhiteSpace($Code) -or $Code.Length -lt 4) {
        return "<redacted>"
    }

    return ($Code.Substring(0, 4) + "****")
}

function Get-ReportDirectoryFromOutput {
    param(
        [object[]]$Output,
        [string]$Prefix
    )

    foreach ($line in $Output) {
        $text = [string]$line
        if ($line -is [System.Management.Automation.InformationRecord]) {
            $text = [string]$line.MessageData
        }

        $match = [regex]::Match($text, "^\[$Prefix\] report: (.+)$")
        if ($match.Success) {
            return $match.Groups[1].Value.Trim()
        }
    }

    return ""
}

function Write-JsonFile {
    param(
        [string]$Path,
        [object]$Value
    )

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    $Value | ConvertTo-Json -Depth 8 | Set-Content -Path $Path -Encoding UTF8
}

function Write-WorkflowResult {
    param(
        [string]$Path,
        [string]$Status,
        [string]$WorkflowResultSummaryPath = "",
        [string]$ReportDirectory = "",
        [string]$SummaryPath = "",
        [string]$ArchiveBundle = "",
        [string]$ArchiveZipPath = "",
        [string]$ArchiveZipSha256 = "",
        [object]$HelperExitCode = $null,
        [object]$SummaryExitCode = $null,
        [object]$ArchiveExitCode = $null,
        [string]$ErrorMessage = "",
        [bool]$ToolchainSelfTestIncluded = $false,
        [bool]$ExistingReportRegressionIncluded = $false
    )

    $workflowResult = [pscustomobject]@{
        schema = "redclaw.dht.role.validation.workflow.v1"
        generated_at = (Get-Date).ToString("o")
        status = $Status
        role = $Role
        mode = if ($DryRun) { "dry_run" } else { "live" }
        session_code_redacted = Get-RedactedSessionCode -Code $SessionCode
        workflow_result_summary = if ([string]::IsNullOrWhiteSpace($WorkflowResultSummaryPath)) { $null } else { $WorkflowResultSummaryPath }
        report_directory = if ([string]::IsNullOrWhiteSpace($ReportDirectory)) { $null } else { $ReportDirectory }
        summary = if ([string]::IsNullOrWhiteSpace($SummaryPath)) { $null } else { $SummaryPath }
        archive = if ([string]::IsNullOrWhiteSpace($ArchiveBundle)) { $null } else { $ArchiveBundle }
        archive_zip = if ([string]::IsNullOrWhiteSpace($ArchiveZipPath)) { $null } else { $ArchiveZipPath }
        archive_zip_sha256 = $ArchiveZipSha256
        helper_exit_code = $HelperExitCode
        summary_exit_code = $SummaryExitCode
        archive_exit_code = $ArchiveExitCode
        error_message = if ([string]::IsNullOrWhiteSpace($ErrorMessage)) { $null } else { $ErrorMessage }
        toolchain_self_test_included = $ToolchainSelfTestIncluded
        existing_report_regression_included = $ExistingReportRegressionIncluded
    }

    Write-JsonFile -Path $Path -Value $workflowResult
    Write-Host "[dht-workflow] workflow_result: $Path"
}

function Write-WorkflowResultSummary {
    param(
        [string]$ScriptPath,
        [string]$WorkflowResultPath,
        [string]$WorkflowResultSummaryPath
    )

    if (-not (Test-Path $ScriptPath)) {
        Write-Host "[dht-workflow] workflow_result_summary skipped: summarizer not found" -ForegroundColor Yellow
        return 2
    }

    & $ScriptPath -WorkflowResult $WorkflowResultPath -OutputPath $WorkflowResultSummaryPath | Out-Host
    $exitCode = $LASTEXITCODE
    Write-Host "[dht-workflow] workflow_result_summary: $WorkflowResultSummaryPath"
    return $exitCode
}

function Invoke-RoleHelper {
    param(
        [string]$ScriptPath,
        [hashtable]$Parameters
    )

    $output = @(& $ScriptPath @Parameters 2>&1 6>&1)
    $exitCode = $LASTEXITCODE
    foreach ($line in $output) {
        Write-Host $line
    }

    return [pscustomobject]@{
        exit_code = $exitCode
        output = $output
    }
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$helperScript = if ($Role -eq "host") {
    Join-Path $PSScriptRoot "run-dht-host-validation.ps1"
} else {
    Join-Path $PSScriptRoot "run-dht-controller-validation.ps1"
}
$summarizerScript = Join-Path $PSScriptRoot "summarize-dht-validation-report.ps1"
$workflowResultSummarizerScript = Join-Path $PSScriptRoot "summarize-dht-workflow-result.ps1"
$archiveScript = Join-Path $PSScriptRoot "archive-dht-validation-evidence.ps1"

$timestamp = "{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss_fff"), ([guid]::NewGuid().ToString("N").Substring(0, 6))
$reportRootAbsolute = Join-Path $repoRoot $ReportRoot
$workflowSummaryPath = Join-Path $reportRootAbsolute ("dht-{0}-workflow-summary-{1}.json" -f $Role, $timestamp)
$workflowResultPath = Join-Path $reportRootAbsolute ("dht-{0}-workflow-result-{1}.json" -f $Role, $timestamp)
$workflowResultSummaryPath = Join-Path $reportRootAbsolute ("dht-{0}-workflow-result-summary-{1}.json" -f $Role, $timestamp)
$bundleName = "dht-{0}-workflow-evidence-{1}" -f $Role, $timestamp

foreach ($scriptPath in @($helperScript, $summarizerScript, $workflowResultSummarizerScript, $archiveScript)) {
    if (-not (Test-Path $scriptPath)) {
        $message = "Required script not found: $scriptPath"
        Write-WorkflowResult -Path $workflowResultPath `
            -Status "setup_failed" `
            -WorkflowResultSummaryPath $workflowResultSummaryPath `
            -SummaryPath $workflowSummaryPath `
            -ErrorMessage $message `
            -ToolchainSelfTestIncluded (-not $SkipArchive -and -not $SkipToolchainSelfTest) `
            -ExistingReportRegressionIncluded ([bool]$IncludeExistingReportRegression)
        if (Test-Path $workflowResultSummarizerScript) {
            Write-WorkflowResultSummary -ScriptPath $workflowResultSummarizerScript `
                -WorkflowResultPath $workflowResultPath `
                -WorkflowResultSummaryPath $workflowResultSummaryPath | Out-Null
        }
        Write-Error $message
        exit 2
    }
}

Write-Host "[dht-workflow] role: $Role"
Write-Host "[dht-workflow] session: $(Get-RedactedSessionCode -Code $SessionCode)"
Write-Host "[dht-workflow] mode: $(if ($DryRun) { 'dry-run' } else { 'live' })"

$helperParameters = @{
    SessionCode = $SessionCode
    Configuration = $Configuration
    ReportRoot = $ReportRoot
    IceServer = $IceServer
    DhtBootstrap = $DhtBootstrap
    DhtPollIntervalMs = $DhtPollIntervalMs
    DhtPublishRetryMs = $DhtPublishRetryMs
    RunSeconds = $RunSeconds
    WaitSeconds = $WaitSeconds
    StopAfterConnectedSeconds = $StopAfterConnectedSeconds
    BootstrapDnsTimeoutMs = $BootstrapDnsTimeoutMs
}

if (-not [string]::IsNullOrWhiteSpace($RuntimeExe)) {
    $helperParameters.RuntimeExe = $RuntimeExe
}
if ($DhtListenPort -gt 0) {
    $helperParameters.DhtListenPort = $DhtListenPort
}
$helperParameters.EnableIceTcp = $EnableIceTcp
$helperParameters.EnablePortMapping = $EnablePortMapping
if ($DisableIpv6Candidates) {
    $helperParameters.DisableIpv6Candidates = $true
}
if ($SkipBootstrapDoh) {
    $helperParameters.SkipBootstrapDoh = $true
}
if ($SkipBootstrapReadiness) {
    $helperParameters.SkipBootstrapReadiness = $true
}
if ($DryRun) {
    $helperParameters.DryRun = $true
}

$prefix = if ($Role -eq "host") { "dht-host" } else { "dht-controller" }
$helperResult = $null
$helperError = ""
try {
    $helperResult = Invoke-RoleHelper -ScriptPath $helperScript -Parameters $helperParameters
} catch {
    $helperError = $_.Exception.Message
    $helperResult = [pscustomobject]@{
        exit_code = 1
        output = @()
    }
}
$reportDirectory = Get-ReportDirectoryFromOutput -Output $helperResult.output -Prefix $prefix
if ([string]::IsNullOrWhiteSpace($reportDirectory)) {
    $status = if ($helperResult.exit_code -ne 0) { "helper_failed" } else { "report_missing" }
    $message = if ([string]::IsNullOrWhiteSpace($helperError)) {
        "Failed to locate helper report directory from $Role helper output."
    } else {
        $helperError
    }
    Write-WorkflowResult -Path $workflowResultPath `
        -Status $status `
        -WorkflowResultSummaryPath $workflowResultSummaryPath `
        -SummaryPath $workflowSummaryPath `
        -HelperExitCode $helperResult.exit_code `
        -ErrorMessage $message `
        -ToolchainSelfTestIncluded (-not $SkipArchive -and -not $SkipToolchainSelfTest) `
        -ExistingReportRegressionIncluded ([bool]$IncludeExistingReportRegression)
    Write-WorkflowResultSummary -ScriptPath $workflowResultSummarizerScript `
        -WorkflowResultPath $workflowResultPath `
        -WorkflowResultSummaryPath $workflowResultSummaryPath | Out-Null
    Write-Error $message
    exit $(if ($helperResult.exit_code -ne 0) { $helperResult.exit_code } else { 1 })
}

Write-Host "[dht-workflow] report: $reportDirectory"

$summaryExitCode = 0
$summaryError = ""
try {
    & $summarizerScript -ReportDirectory $reportDirectory -OutputPath $workflowSummaryPath | Out-Host
    $summaryExitCode = $LASTEXITCODE
} catch {
    $summaryExitCode = 1
    $summaryError = $_.Exception.Message
}
Write-Host "[dht-workflow] summary: $workflowSummaryPath"

$archiveExitCode = 0
$archiveBundle = ""
$archiveZipPath = ""
$archiveZipSha256 = ""
$archiveError = ""
if (-not $SkipArchive) {
    $archiveParameters = @{
        ReportDirectory = @($reportDirectory)
        BundleName = $bundleName
    }
    if ($CreateZip) {
        $archiveParameters.CreateZip = $true
    }
    if (-not $SkipToolchainSelfTest) {
        $archiveParameters.IncludeToolchainSelfTest = $true
    }
    if ($IncludeExistingReportRegression) {
        $archiveParameters.IncludeExistingReportRegression = $true
    }

    try {
        & $archiveScript @archiveParameters | Out-Host
        $archiveExitCode = $LASTEXITCODE
    } catch {
        $archiveExitCode = 1
        $archiveError = $_.Exception.Message
    }
    $archiveBundle = Join-Path $reportRootAbsolute $bundleName
    if ($CreateZip) {
        $archiveZipPath = "$archiveBundle.zip"
        if (Test-Path $archiveZipPath) {
            $archiveZipSha256 = (Get-FileHash -Algorithm SHA256 -Path $archiveZipPath).Hash.ToLowerInvariant()
        }
    }
    Write-Host "[dht-workflow] archive: $archiveBundle"
}

$workflowStatus = if ($helperResult.exit_code -eq 0 -and $summaryExitCode -eq 0 -and $archiveExitCode -eq 0) {
    "success"
} elseif ($helperResult.exit_code -ne 0) {
    "helper_failed"
} elseif ($summaryExitCode -ne 0) {
    "summary_failed"
} elseif ($archiveExitCode -ne 0) {
    "archive_failed"
} else {
    "unknown"
}

$errorMessage = (@($helperError, $summaryError, $archiveError) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join "; "
Write-WorkflowResult -Path $workflowResultPath `
    -Status $workflowStatus `
    -WorkflowResultSummaryPath $workflowResultSummaryPath `
    -ReportDirectory $reportDirectory `
    -SummaryPath $workflowSummaryPath `
    -ArchiveBundle $archiveBundle `
    -ArchiveZipPath $archiveZipPath `
    -ArchiveZipSha256 $archiveZipSha256 `
    -HelperExitCode $helperResult.exit_code `
    -SummaryExitCode $summaryExitCode `
    -ArchiveExitCode $archiveExitCode `
    -ErrorMessage $errorMessage `
    -ToolchainSelfTestIncluded (-not $SkipArchive -and -not $SkipToolchainSelfTest) `
    -ExistingReportRegressionIncluded ([bool]$IncludeExistingReportRegression)

Write-WorkflowResultSummary -ScriptPath $workflowResultSummarizerScript `
    -WorkflowResultPath $workflowResultPath `
    -WorkflowResultSummaryPath $workflowResultSummaryPath | Out-Null

if ($helperResult.exit_code -ne 0) {
    exit $helperResult.exit_code
}
if ($summaryExitCode -ne 0) {
    exit $summaryExitCode
}
if ($archiveExitCode -ne 0) {
    exit $archiveExitCode
}

exit 0
