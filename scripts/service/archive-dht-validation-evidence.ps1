param(
    [Parameter(Mandatory = $true)]
    [string[]]$ReportDirectory,

    [string]$OutputRoot = "build\reports",

    [string]$BundleName = "",

    [switch]$CreateZip,

    [string]$ZipPath = "",

    [switch]$IncludeToolchainSelfTest,

    [switch]$IncludeExistingReportRegression,

    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $candidate = Resolve-Path (Join-Path $PSScriptRoot "..\..")
    return $candidate.Path
}

function Expand-ReportDirectoryArguments {
    param([string[]]$Values)

    $expanded = @()
    foreach ($value in $Values) {
        foreach ($part in ([string]$value -split ",")) {
            if (-not [string]::IsNullOrWhiteSpace($part)) {
                $expanded += $part.Trim()
            }
        }
    }

    return $expanded
}

function Resolve-ReportDirectory {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Report directory not found: $Path"
    }

    $item = Get-Item -Path $Path
    if (-not $item.PSIsContainer) {
        throw "Report path is not a directory: $Path"
    }

    return $item.FullName
}

function Get-RelativePath {
    param(
        [string]$BasePath,
        [string]$Path
    )

    $baseUri = [System.Uri]((Resolve-Path $BasePath).Path.TrimEnd('\') + '\')
    $pathUri = [System.Uri]((Resolve-Path $Path).Path)
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($pathUri).ToString()).Replace('/', '\')
}

function Get-DirectoryFilesManifest {
    param(
        [string]$BundleRoot,
        [string]$Directory
    )

    $files = @()
    foreach ($file in (Get-ChildItem -Path $Directory -File -Recurse | Sort-Object FullName)) {
        $hash = Get-FileHash -Algorithm SHA256 -Path $file.FullName
        $files += [pscustomobject]@{
            relative_path = Get-RelativePath -BasePath $BundleRoot -Path $file.FullName
            size_bytes = $file.Length
            sha256 = $hash.Hash.ToLowerInvariant()
            last_write_time = $file.LastWriteTime.ToString("o")
        }
    }

    return $files
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$summarizerScript = Join-Path $PSScriptRoot "summarize-dht-validation-report.ps1"
if (-not (Test-Path $summarizerScript)) {
    throw "Required summarizer script not found: $summarizerScript"
}

$toolchainSelfTestScript = Join-Path $PSScriptRoot "test-dht-validation-toolchain.ps1"
if ($IncludeToolchainSelfTest -and -not (Test-Path $toolchainSelfTestScript)) {
    throw "Required toolchain self-test script not found: $toolchainSelfTestScript"
}

$resolvedReports = @()
foreach ($directory in (Expand-ReportDirectoryArguments -Values $ReportDirectory)) {
    $resolvedReports += Resolve-ReportDirectory -Path $directory
}

if ($resolvedReports.Count -eq 0) {
    throw "At least one report directory is required."
}

if ([string]::IsNullOrWhiteSpace($BundleName)) {
    $runId = "{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss_fff"), ([guid]::NewGuid().ToString("N").Substring(0, 6))
    $BundleName = "dht-validation-evidence-{0}" -f $runId
}

$outputRootPath = Join-Path $repoRoot $OutputRoot
$bundleRoot = Join-Path $outputRootPath $BundleName
$reportsRoot = Join-Path $bundleRoot "reports"
$summaryPath = Join-Path $bundleRoot "dht-summary.json"
$manifestPath = Join-Path $bundleRoot "archive-manifest.json"
if ($CreateZip -and [string]::IsNullOrWhiteSpace($ZipPath)) {
    $ZipPath = "$bundleRoot.zip"
} elseif ($CreateZip -and -not [System.IO.Path]::IsPathRooted($ZipPath)) {
    $ZipPath = Join-Path $repoRoot $ZipPath
}

Write-Host "[dht-archive] bundle: $bundleRoot"
foreach ($report in $resolvedReports) {
    Write-Host "[dht-archive] include: $report"
}
if ($CreateZip) {
    Write-Host "[dht-archive] zip: $ZipPath"
}
if ($IncludeToolchainSelfTest) {
    Write-Host "[dht-archive] include toolchain self-test: true"
}

if ($DryRun) {
    Write-Host "[dht-archive] dry run only; no files were copied."
    exit 0
}

New-Item -ItemType Directory -Force -Path $reportsRoot | Out-Null

$copiedReports = @()
$copiedReportDirectories = @()
foreach ($report in $resolvedReports) {
    $leaf = Split-Path -Leaf $report
    $destination = Join-Path $reportsRoot $leaf
    Copy-Item -Path $report -Destination $destination -Recurse -Force
    $copiedReportDirectories += $destination
    $copiedReports += [pscustomobject]@{
        source = $report
        archived = $destination
    }
}

& $summarizerScript -ReportDirectory $copiedReportDirectories -OutputPath $summaryPath | Out-Host
$summaryExitCode = $LASTEXITCODE

$toolchainSelfTest = $null
if ($IncludeToolchainSelfTest) {
    $selfTestRoot = Join-Path $bundleRoot "toolchain-selftest"
    $selfTestSummaryPath = Join-Path $bundleRoot "toolchain-selftest-summary.json"
    $selfTestParameters = @{
        ReportRoot = $selfTestRoot
        OutputPath = $selfTestSummaryPath
    }
    if ($IncludeExistingReportRegression) {
        $selfTestParameters.IncludeExistingReportRegression = $true
    }

    & $toolchainSelfTestScript @selfTestParameters | Out-Host
    $selfTestExitCode = $LASTEXITCODE
    if ($selfTestExitCode -ne 0) {
        throw "DHT validation toolchain self-test failed with exit code $selfTestExitCode"
    }
    $toolchainSelfTest = [pscustomobject]@{
        summary = $selfTestSummaryPath
        report_root = $selfTestRoot
        include_existing_report_regression = [bool]$IncludeExistingReportRegression
    }
}

$manifest = [pscustomobject]@{
    schema = "redclaw.dht.validation.evidence.v1"
    generated_at = (Get-Date).ToString("o")
    bundle_root = $bundleRoot
    report_count = $resolvedReports.Count
    reports = $copiedReports
    summary = $summaryPath
    toolchain_self_test = $toolchainSelfTest
    files = Get-DirectoryFilesManifest -BundleRoot $bundleRoot -Directory $bundleRoot
}

$manifest | ConvertTo-Json -Depth 6 | Set-Content -Path $manifestPath -Encoding UTF8

Write-Host "[dht-archive] summary: $summaryPath"
if ($null -ne $toolchainSelfTest) {
    Write-Host "[dht-archive] toolchain_self_test: $($toolchainSelfTest.summary)"
}
Write-Host "[dht-archive] manifest: $manifestPath"

if ($CreateZip) {
    if (Test-Path $ZipPath) {
        throw "Zip output already exists: $ZipPath"
    }

    $zipParent = Split-Path -Parent $ZipPath
    if (-not [string]::IsNullOrWhiteSpace($zipParent)) {
        New-Item -ItemType Directory -Force -Path $zipParent | Out-Null
    }

    Compress-Archive -Path (Join-Path $bundleRoot "*") -DestinationPath $ZipPath
    $zipHash = Get-FileHash -Algorithm SHA256 -Path $ZipPath
    Write-Host "[dht-archive] zip written: $ZipPath"
    Write-Host "[dht-archive] zip_sha256: $($zipHash.Hash.ToLowerInvariant())"
}

exit $summaryExitCode
