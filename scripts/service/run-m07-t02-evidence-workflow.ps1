param(
    [ValidateSet("pre-reboot", "post-reboot")]
    [string]$Stage = "pre-reboot",

    [string]$RunId = "",

    [string]$ServiceName = "RedClawDesktopM07T02Smoke",

    [string]$ServiceDisplayName = "RedClawDesktop M07 T02 Boot Validation",

    [string]$HostServiceBinaryPath = "",

    [string]$ReportsRoot = "build/reports",

    [int]$StartTimeoutSeconds = 30,

    [switch]$InstallIfMissing,

    [switch]$KeepServiceAfterRun,

    [switch]$AllowMissingReferenceForPostReboot,

    [switch]$ArchiveNonStrict
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue,
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }

    return Join-Path $RepoRoot $PathValue
}

function Assert-CommandStatus {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OperationName,
        [Parameter(Mandatory = $true)]
        [bool]$Succeeded,
        [AllowNull()]
        [int]$ExitCode
    )

    if ($Succeeded) {
        return
    }

    if ($null -ne $ExitCode -and $ExitCode -ne 0) {
        throw "$OperationName failed with exit code $ExitCode"
    }

    throw "$OperationName failed. Check command output for details."
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptRoot)

$runScriptPath = Join-Path $scriptRoot "run-m07-t02-boot-validation.ps1"
$collectScriptPath = Join-Path $scriptRoot "collect-m07-t02-post-reboot-evidence.ps1"
$archiveScriptPath = Join-Path $scriptRoot "archive-m07-t02-boot-evidence.ps1"

foreach ($path in @($runScriptPath, $collectScriptPath, $archiveScriptPath)) {
    if (-not (Test-Path -Path $path -PathType Leaf)) {
        throw "Required script not found: $path"
    }
}

$resolvedReportsRoot = Resolve-RepoPath -PathValue $ReportsRoot -RepoRoot $repoRoot
New-Item -ItemType Directory -Force -Path $resolvedReportsRoot | Out-Null

$contextPath = Join-Path $resolvedReportsRoot "m07-t02-workflow-context.json"

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "m07-t02-" + (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
}

$baselineFileName = "m07-t02-boot-validation-$RunId.json"
$postRebootFileName = "m07-t02-post-reboot-evidence-$RunId.json"
$baselinePath = Join-Path $resolvedReportsRoot $baselineFileName
$postRebootPath = Join-Path $resolvedReportsRoot $postRebootFileName

if ($Stage -eq "pre-reboot") {
    $runArgs = @{
        ServiceName = $ServiceName
        ServiceDisplayName = $ServiceDisplayName
        OutputJsonPath = $baselinePath
        StartTimeoutSeconds = $StartTimeoutSeconds
    }

    if (-not [string]::IsNullOrWhiteSpace($HostServiceBinaryPath)) {
        $runArgs.HostServiceBinaryPath = $HostServiceBinaryPath
    }

    if ($InstallIfMissing) {
        $runArgs.InstallIfMissing = $true
    }

    if ($KeepServiceAfterRun) {
        $runArgs.KeepServiceAfterRun = $true
    }

    & $runScriptPath @runArgs
    $preRebootSucceeded = $?
    $preRebootExitCode = $LASTEXITCODE
    Assert-CommandStatus -OperationName "Pre-reboot baseline validation" -Succeeded $preRebootSucceeded -ExitCode $preRebootExitCode

    $context = [ordered]@{
        run_id = $RunId
        stage_completed = "pre-reboot"
        service_name = $ServiceName
        baseline_path = $baselinePath
        post_reboot_path = $postRebootPath
        reports_root = $resolvedReportsRoot
        created_utc = (Get-Date).ToUniversalTime().ToString("o")
    }

    $context | ConvertTo-Json -Depth 6 | Out-File -FilePath $contextPath -Encoding utf8

    Write-Host "Workflow context file: $contextPath"
    Write-Host "Run ID: $RunId"
    Write-Host "Next step: reboot machine, then run post-reboot stage:"
    Write-Host "powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-m07-t02-evidence-workflow.ps1 -Stage post-reboot -RunId $RunId"
    exit 0
}

$contextData = $null
if (Test-Path -Path $contextPath -PathType Leaf) {
    $contextData = Get-Content -Raw -Path $contextPath | ConvertFrom-Json
}

if ($null -ne $contextData -and -not [string]::IsNullOrWhiteSpace([string]$contextData.run_id) -and [string]$contextData.run_id -eq $RunId) {
    if ($contextData.PSObject.Properties.Name -contains "baseline_path") {
        $baselinePath = [string]$contextData.baseline_path
        $baselineFileName = [System.IO.Path]::GetFileName($baselinePath)
    }

    if ($contextData.PSObject.Properties.Name -contains "post_reboot_path") {
        $postRebootPath = [string]$contextData.post_reboot_path
        $postRebootFileName = [System.IO.Path]::GetFileName($postRebootPath)
    }

    if ($contextData.PSObject.Properties.Name -contains "service_name") {
        $ServiceName = [string]$contextData.service_name
    }
}

$collectArgs = @{
    ServiceName = $ServiceName
    ReferenceValidationPath = $baselinePath
    OutputJsonPath = $postRebootPath
}

if ($AllowMissingReferenceForPostReboot) {
    $collectArgs.AllowMissingReference = $true
}

& $collectScriptPath @collectArgs
$collectSucceeded = $?
$collectExitCode = $LASTEXITCODE
Assert-CommandStatus -OperationName "Post-reboot evidence collection" -Succeeded $collectSucceeded -ExitCode $collectExitCode

$archiveArgs = @{
    RunId = $RunId
    SourceRoot = $resolvedReportsRoot
    BaselineFileName = $baselineFileName
    PostRebootFileName = $postRebootFileName
}

if (-not $ArchiveNonStrict) {
    $archiveArgs.Strict = $true
}

& $archiveScriptPath @archiveArgs
$archiveSucceeded = $?
$archiveExitCode = $LASTEXITCODE
Assert-CommandStatus -OperationName "Evidence archive" -Succeeded $archiveSucceeded -ExitCode $archiveExitCode

Write-Host "Workflow complete for Run ID: $RunId"
exit 0
