param(
    [string]$ServiceName = "RedClawDesktopM07T02Smoke",

    [string]$ReferenceValidationPath = "build/reports/m07-t02-boot-validation.json",

    [string]$OutputJsonPath = "build/reports/m07-t02-post-reboot-evidence.json",

    [switch]$AllowMissingReference
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

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptRoot)

$resolvedReferencePath = Resolve-RepoPath -PathValue $ReferenceValidationPath -RepoRoot $repoRoot
$resolvedOutputPath = Resolve-RepoPath -PathValue $OutputJsonPath -RepoRoot $repoRoot

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedOutputPath) | Out-Null

$result = [ordered]@{
    passed = $false
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    service_name = $ServiceName
    reference_validation_path = $resolvedReferencePath
    output_path = $resolvedOutputPath
    checks = [ordered]@{
        reference_report_exists_or_allowed = $false
        reference_report_passed = $false
        service_exists = $false
        startup_mode_auto = $false
        service_running = $false
        reboot_after_reference = $false
    }
    details = [ordered]@{
        reference_completed_utc = ""
        current_boot_utc = ""
        startup_mode = ""
        service_state = ""
        uptime_seconds = 0
    }
    error = ""
}

try {
    $referenceCompletedUtc = $null

    if (Test-Path -Path $resolvedReferencePath -PathType Leaf) {
        $result.checks.reference_report_exists_or_allowed = $true

        $referenceRaw = Get-Content -Raw -Path $resolvedReferencePath
        $reference = $referenceRaw | ConvertFrom-Json

        if ($null -ne $reference -and $reference.passed -eq $true) {
            $result.checks.reference_report_passed = $true
        }

        if ($null -ne $reference -and $reference.PSObject.Properties.Name -contains "completed_utc") {
            $completedText = [string]$reference.completed_utc
            if (-not [string]::IsNullOrWhiteSpace($completedText)) {
                $referenceCompletedUtc = [DateTimeOffset]::Parse($completedText).ToUniversalTime()
                $result.details.reference_completed_utc = $referenceCompletedUtc.ToString("o")
            }
        }
    }
    elseif ($AllowMissingReference) {
        $result.checks.reference_report_exists_or_allowed = $true
        $result.checks.reference_report_passed = $true
    }
    else {
        throw "Reference validation report not found: $resolvedReferencePath"
    }

    $escapedName = $ServiceName.Replace("'", "''")
    $serviceInfo = Get-CimInstance -ClassName Win32_Service -Filter "Name='$escapedName'" -ErrorAction SilentlyContinue
    if ($null -eq $serviceInfo) {
        throw "Service '$ServiceName' not found."
    }

    $result.checks.service_exists = $true

    $startMode = ([string]$serviceInfo.StartMode).ToUpperInvariant()
    $state = ([string]$serviceInfo.State).ToUpperInvariant()

    $result.details.startup_mode = $startMode
    $result.details.service_state = $state

    if ($startMode -eq "AUTO" -or $startMode -eq "AUTOMATIC") {
        $result.checks.startup_mode_auto = $true
    }

    if ($state -eq "RUNNING") {
        $result.checks.service_running = $true
    }

    $osInfo = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop

    $bootTimeUtc = $null
    if ($osInfo.LastBootUpTime -is [datetime]) {
        $bootDateTime = ([datetime]$osInfo.LastBootUpTime).ToUniversalTime()
        $bootTimeUtc = [DateTimeOffset]::new($bootDateTime, [TimeSpan]::Zero)
    }
    else {
        $bootText = [string]$osInfo.LastBootUpTime
        if ([string]::IsNullOrWhiteSpace($bootText)) {
            throw "Unable to resolve LastBootUpTime from Win32_OperatingSystem."
        }

        $bootDateTime = [Management.ManagementDateTimeConverter]::ToDateTime($bootText).ToUniversalTime()
        $bootTimeUtc = [DateTimeOffset]::new($bootDateTime, [TimeSpan]::Zero)
    }

    $nowUtc = [DateTimeOffset]::UtcNow

    $result.details.current_boot_utc = $bootTimeUtc.ToString("o")
    $result.details.uptime_seconds = [int64]($nowUtc - $bootTimeUtc).TotalSeconds

    if ($null -eq $referenceCompletedUtc) {
        $result.checks.reboot_after_reference = $true
    }
    else {
        $result.checks.reboot_after_reference = ($bootTimeUtc -gt $referenceCompletedUtc)
    }

    $result.passed = $result.checks.reference_report_exists_or_allowed `
        -and $result.checks.reference_report_passed `
        -and $result.checks.service_exists `
        -and $result.checks.startup_mode_auto `
        -and $result.checks.service_running `
        -and $result.checks.reboot_after_reference
}
catch {
    $result.error = $_.Exception.Message
}
finally {
    $result.completed_utc = (Get-Date).ToUniversalTime().ToString("o")
    $result | ConvertTo-Json -Depth 8 | Out-File -FilePath $resolvedOutputPath -Encoding utf8

    Write-Host "Post-reboot evidence file: $resolvedOutputPath"
    if ($result.passed) {
        Write-Host "Result: PASS"
        exit 0
    }

    if (-not [string]::IsNullOrWhiteSpace($result.error)) {
        Write-Host "Result: FAIL"
        Write-Host "Error: $($result.error)"
        exit 1
    }

    Write-Host "Result: FAIL (check report JSON for details)"
    exit 1
}
