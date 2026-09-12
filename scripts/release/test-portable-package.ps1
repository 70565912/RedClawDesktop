param(
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,

    [Parameter(Mandatory = $true)]
    [string]$ChecksumPath,

    [string]$OutputJsonPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolvedArchive = (Resolve-Path -LiteralPath $ArchivePath).Path
$resolvedChecksum = (Resolve-Path -LiteralPath $ChecksumPath).Path
$archiveName = Split-Path -Leaf $resolvedArchive
$archiveHash = (Get-FileHash -LiteralPath $resolvedArchive -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumText = Get-Content -LiteralPath $resolvedChecksum -Raw
$hashPattern = '(?im)^(?<hash>[0-9a-f]{64})\s+' + [regex]::Escape($archiveName) + '\s*$'
$checksumMatch = [regex]::Match($checksumText, $hashPattern)
if (-not $checksumMatch.Success) {
    throw "Checksum entry missing for $archiveName"
}
$expectedHash = $checksumMatch.Groups['hash'].Value.ToLowerInvariant()
if ($archiveHash -ne $expectedHash) {
    throw "Archive SHA256 mismatch: expected $expectedHash, got $archiveHash"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($resolvedArchive)
try {
    $entryNames = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    $unsafeEntries = @($entryNames | Where-Object {
        $_.StartsWith('/') -or $_ -match '^[A-Za-z]:' -or $_ -match '(^|/)\.\.(/|$)'
    })
    if ($unsafeEntries.Count -gt 0) {
        throw "Archive contains unsafe paths:`n$($unsafeEntries -join "`n")"
    }
} finally {
    $archive.Dispose()
}

$requiredEntries = @(
    'RedClawDesktop/redclaw_desktop.exe',
    'RedClawDesktop/platforms/qwindows.dll',
    'RedClawDesktop/README.md',
    'RedClawDesktop/README.en.md',
    'RedClawDesktop/LICENSE',
    'RedClawDesktop/RELEASE-MANIFEST.txt'
)
foreach ($requiredEntry in $requiredEntries) {
    if ($entryNames -notcontains $requiredEntry) {
        throw "Required archive entry is missing: $requiredEntry"
    }
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'RedClawDesktop-package-smoke-' + $PID + '-' + [guid]::NewGuid().ToString('N'))
$result = $null
try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    Expand-Archive -LiteralPath $resolvedArchive -DestinationPath $temporaryRoot
    $packageRoot = Join-Path $temporaryRoot 'RedClawDesktop'
    $executablePath = Join-Path $packageRoot 'redclaw_desktop.exe'

    $standardOutputPath = Join-Path $temporaryRoot 'help-stdout.txt'
    $standardErrorPath = Join-Path $temporaryRoot 'help-stderr.txt'
    $process = Start-Process `
        -FilePath $executablePath `
        -ArgumentList '--help' `
        -WorkingDirectory $packageRoot `
        -RedirectStandardOutput $standardOutputPath `
        -RedirectStandardError $standardErrorPath `
        -PassThru `
        -Wait
    $helpExitCode = $process.ExitCode
    $helpText = @(
        Get-Content -LiteralPath $standardOutputPath -Raw -ErrorAction SilentlyContinue
        Get-Content -LiteralPath $standardErrorPath -Raw -ErrorAction SilentlyContinue
    ) -join "`n"
    $helpText = $helpText.Trim()
    $helpHasUsage = $helpText.Contains('Usage: redclaw_desktop [options]')
    $executableHash = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $passed = $helpExitCode -eq 0 -and $helpHasUsage
    $result = [ordered]@{
        archive_name = $archiveName
        archive_sha256 = $archiveHash
        executable_sha256 = $executableHash
        archive_file_count = @($entryNames | Where-Object { -not $_.EndsWith('/') }).Count
        help_exit_code = $helpExitCode
        help_output_has_usage = $helpHasUsage
        passed = $passed
    }
} finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

if (-not [string]::IsNullOrWhiteSpace($OutputJsonPath)) {
    $outputDirectory = Split-Path -Parent $OutputJsonPath
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
        New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    }
    $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $OutputJsonPath -Encoding utf8
}

$result | ConvertTo-Json -Depth 4
if (-not $result.passed) {
    throw 'Portable package command-entry smoke check failed.'
}
