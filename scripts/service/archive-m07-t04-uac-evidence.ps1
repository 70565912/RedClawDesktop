param(
    [string]$RunId = "",
    [string]$SourceRoot = "build/reports",
    [string]$DestinationRoot = "build/reports/m07-t04-runs",
    [string]$ManifestFileName = "evidence-manifest.json",
    [switch]$Strict
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

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = "m07-t04-" + (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
}

$resolvedSourceRoot = Resolve-RepoPath -PathValue $SourceRoot
$resolvedDestinationRoot = Resolve-RepoPath -PathValue $DestinationRoot
$destinationRunDir = Join-Path $resolvedDestinationRoot $RunId

$expectedFiles = @(
    "m07-d01-scenario-a.json",
    "m07-d02-scenario-b.json",
    "m07-d02-scenario-c.json",
    "m07-d03-audit-validation.json",
    "m07-t04-uac-e2e-validation.json"
)

New-Item -ItemType Directory -Force -Path $destinationRunDir | Out-Null

$copiedFiles = @()
$missingFiles = @()

foreach ($fileName in $expectedFiles) {
    $sourcePath = Join-Path $resolvedSourceRoot $fileName
    if (-not (Test-Path -Path $sourcePath -PathType Leaf)) {
        $missingFiles += $fileName
        continue
    }

    $destinationPath = Join-Path $destinationRunDir $fileName
    Copy-Item -Path $sourcePath -Destination $destinationPath -Force

    $fileHash = (Get-FileHash -Path $destinationPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $fileInfo = Get-Item -Path $destinationPath

    $copiedFiles += [ordered]@{
        name = $fileName
        source_path = $sourcePath
        archived_path = $destinationPath
        size_bytes = $fileInfo.Length
        last_write_time_utc = $fileInfo.LastWriteTimeUtc.ToString("o")
        sha256 = $fileHash
    }
}

$manifestPath = Join-Path $destinationRunDir $ManifestFileName

$manifest = [ordered]@{
    run_id = $RunId
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    source_root = $resolvedSourceRoot
    archive_root = $resolvedDestinationRoot
    archive_directory = $destinationRunDir
    strict_mode = [bool]$Strict
    copied_count = $copiedFiles.Count
    missing_count = $missingFiles.Count
    copied_files = $copiedFiles
    missing_files = $missingFiles
}

$manifest | ConvertTo-Json -Depth 8 | Out-File -FilePath $manifestPath -Encoding utf8

Write-Host "Archive directory: $destinationRunDir"
Write-Host "Manifest: $manifestPath"

if ($Strict -and $missingFiles.Count -gt 0) {
    Write-Host "Result: FAIL (strict mode, missing files: $($missingFiles -join ', '))"
    exit 1
}

if ($copiedFiles.Count -eq 0) {
    Write-Host "Result: FAIL (no evidence files found to archive)"
    exit 1
}

if ($missingFiles.Count -gt 0) {
    Write-Host "Result: PASS_WITH_WARNINGS (missing files: $($missingFiles -join ', '))"
    exit 0
}

Write-Host "Result: PASS"
exit 0
