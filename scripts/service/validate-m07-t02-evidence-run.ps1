param(
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,

    [string]$ManifestFileName = "evidence-manifest.json",

    [string]$BaselineFileNamePattern = "m07-t02-boot-validation*.json",

    [string]$PostRebootFileNamePattern = "m07-t02-post-reboot-evidence*.json",

    [string]$OutputJsonPath = "",

    [switch]$AllowWarnings
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

$resolvedRunDirectory = Resolve-RepoPath -PathValue $RunDirectory -RepoRoot $repoRoot
if (-not (Test-Path -Path $resolvedRunDirectory -PathType Container)) {
    throw "Run directory not found: $resolvedRunDirectory"
}

if ([string]::IsNullOrWhiteSpace($OutputJsonPath)) {
    $OutputJsonPath = Join-Path $resolvedRunDirectory "validation-report.json"
}

$resolvedOutputPath = Resolve-RepoPath -PathValue $OutputJsonPath -RepoRoot $repoRoot
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedOutputPath) | Out-Null

$manifestPath = Join-Path $resolvedRunDirectory $ManifestFileName

$result = [ordered]@{
    passed = $false
    allow_warnings = [bool]$AllowWarnings
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    run_directory = $resolvedRunDirectory
    checks = [ordered]@{
        manifest_exists = $false
        manifest_parsed = $false
        baseline_file_present = $false
        post_reboot_file_present = $false
        manifest_hashes_match = $true
        baseline_report_passed = $false
        post_reboot_report_passed = $false
    }
    details = [ordered]@{
        baseline_file = ""
        post_reboot_file = ""
        baseline_completed_utc = ""
        post_reboot_completed_utc = ""
        warnings = @()
    }
    error = ""
}

try {
    if (Test-Path -Path $manifestPath -PathType Leaf) {
        $result.checks.manifest_exists = $true
    }
    else {
        throw "Manifest file not found: $manifestPath"
    }

    $manifest = Get-Content -Raw -Path $manifestPath | ConvertFrom-Json
    if ($null -eq $manifest) {
        throw "Failed to parse manifest JSON: $manifestPath"
    }

    $result.checks.manifest_parsed = $true

    $baselineFile = Get-ChildItem -Path $resolvedRunDirectory -File -Filter $BaselineFileNamePattern |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    $postRebootFile = Get-ChildItem -Path $resolvedRunDirectory -File -Filter $PostRebootFileNamePattern |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if ($null -ne $baselineFile) {
        $result.checks.baseline_file_present = $true
        $result.details.baseline_file = $baselineFile.FullName
    }

    if ($null -ne $postRebootFile) {
        $result.checks.post_reboot_file_present = $true
        $result.details.post_reboot_file = $postRebootFile.FullName
    }

    if (-not $result.checks.baseline_file_present) {
        $result.details.warnings += "Baseline file missing ($BaselineFileNamePattern)"
    }

    if (-not $result.checks.post_reboot_file_present) {
        $result.details.warnings += "Post-reboot file missing ($PostRebootFileNamePattern)"
    }

    $manifestHashes = @{}
    if ($manifest.PSObject.Properties.Name -contains "copied_files") {
        foreach ($entry in $manifest.copied_files) {
            if ($entry.PSObject.Properties.Name -contains "name" -and $entry.PSObject.Properties.Name -contains "sha256") {
                $manifestHashes[[string]$entry.name] = ([string]$entry.sha256).ToLowerInvariant()
            }
        }
    }

    foreach ($file in @($baselineFile, $postRebootFile)) {
        if ($null -eq $file) {
            continue
        }

        $actualHash = (Get-FileHash -Path $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($manifestHashes.ContainsKey($file.Name)) {
            if ($manifestHashes[$file.Name] -ne $actualHash) {
                $result.checks.manifest_hashes_match = $false
                $result.details.warnings += "Manifest hash mismatch for $($file.Name)"
            }
        }
        else {
            $result.details.warnings += "File not listed in manifest copied_files: $($file.Name)"
        }
    }

    if ($null -ne $baselineFile) {
        $baselineJson = Get-Content -Raw -Path $baselineFile.FullName | ConvertFrom-Json
        if ($null -ne $baselineJson -and $baselineJson.passed -eq $true) {
            $result.checks.baseline_report_passed = $true
        }

        if ($null -ne $baselineJson -and $baselineJson.PSObject.Properties.Name -contains "completed_utc") {
            $result.details.baseline_completed_utc = [string]$baselineJson.completed_utc
        }
    }

    if ($null -ne $postRebootFile) {
        $postRebootJson = Get-Content -Raw -Path $postRebootFile.FullName | ConvertFrom-Json
        if ($null -ne $postRebootJson -and $postRebootJson.passed -eq $true) {
            $result.checks.post_reboot_report_passed = $true
        }

        if ($null -ne $postRebootJson -and $postRebootJson.PSObject.Properties.Name -contains "completed_utc") {
            $result.details.post_reboot_completed_utc = [string]$postRebootJson.completed_utc
        }
    }

    $hardChecks = $result.checks.manifest_exists `
        -and $result.checks.manifest_parsed `
        -and $result.checks.baseline_file_present `
        -and $result.checks.post_reboot_file_present `
        -and $result.checks.baseline_report_passed `
        -and $result.checks.post_reboot_report_passed

    $triageChecks = $result.checks.manifest_exists `
        -and $result.checks.manifest_parsed `
        -and ($result.checks.baseline_file_present -or $result.checks.post_reboot_file_present) `
        -and ($result.checks.baseline_report_passed -or $result.checks.post_reboot_report_passed)

    if (-not $result.checks.manifest_hashes_match) {
        if ($AllowWarnings) {
            $result.details.warnings += "Proceeding despite manifest hash mismatch because -AllowWarnings is set"
        }
        else {
            $hardChecks = $false
        }
    }

    if ($AllowWarnings -and $result.details.warnings.Count -gt 0) {
        $result.passed = $triageChecks
    }
    elseif ($AllowWarnings) {
        $result.passed = $triageChecks
    }
    else {
        $result.passed = $hardChecks -and ($result.details.warnings.Count -eq 0)
    }
}
catch {
    $result.error = $_.Exception.Message
}
finally {
    $result.completed_utc = (Get-Date).ToUniversalTime().ToString("o")
    $result | ConvertTo-Json -Depth 8 | Out-File -FilePath $resolvedOutputPath -Encoding utf8

    Write-Host "M07-T02 evidence validation report: $resolvedOutputPath"

    if ($result.passed) {
        if ($result.details.warnings.Count -gt 0) {
            Write-Host "Result: PASS_WITH_WARNINGS"
        }
        else {
            Write-Host "Result: PASS"
        }
        exit 0
    }

    if (-not [string]::IsNullOrWhiteSpace($result.error)) {
        Write-Host "Result: FAIL"
        Write-Host "Error: $($result.error)"
        exit 1
    }

    Write-Host "Result: FAIL (check $resolvedOutputPath)"
    exit 1
}
