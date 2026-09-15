param(
    [string]$CacheDirectory = '',
    [string]$DeployDirectory = '',
    [string]$OutputJsonPath = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$manifest = Get-Content -LiteralPath (Join-Path $repoRoot 'third_party/terminal/manifest.json') -Raw | ConvertFrom-Json
if ($manifest.schema_version -ne 1 -or $manifest.runtime.architecture -ne 'x64') {
    throw 'Unsupported terminal dependency manifest.'
}
if ([string]::IsNullOrWhiteSpace($CacheDirectory)) {
    $CacheDirectory = Join-Path $repoRoot 'build/dependencies/terminal'
}
$cache = [IO.Path]::GetFullPath($CacheDirectory)
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$runtime = $manifest.runtime
$archivePath = Join-Path $cache ('webview2-runtime-' + $runtime.version + '-x64.cab')
if (-not (Test-Path -LiteralPath $archivePath)) {
    $downloadPath = $archivePath + '.download'
    Invoke-WebRequest -Uri $runtime.url -OutFile $downloadPath -UseBasicParsing -TimeoutSec 600
    if ((Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash -ine $runtime.sha256) {
        throw 'Downloaded WebView2 runtime hash mismatch.'
    }
    Move-Item -LiteralPath $downloadPath -Destination $archivePath
}
if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ine $runtime.sha256) {
    throw 'Cached WebView2 archive hash mismatch.'
}
$extractionRoot = Join-Path $cache ('runtime-' + $runtime.version)
$runtimeRoot = Join-Path $extractionRoot ('Microsoft.WebView2.FixedVersionRuntime.' + $runtime.version + '.x64')
$receiptPath = Join-Path $extractionRoot 'verified-files.json'
if (-not (Test-Path -LiteralPath $receiptPath)) {
    # Re-expand from the verified archive before trusting a pre-existing cache.
    New-Item -ItemType Directory -Force -Path $extractionRoot | Out-Null
    $expandLog = Join-Path $extractionRoot 'expand.log'
    & (Join-Path $env:SystemRoot 'System32/expand.exe') $archivePath '-F:*' $extractionRoot *> $expandLog
    if ($LASTEXITCODE -ne 0) { throw "WebView2 extraction failed: $LASTEXITCODE" }
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $runtimeRoot 'msedgewebview2.exe')
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(?:,|$)') {
        throw 'WebView2 runtime Microsoft signature verification failed.'
    }
    $files = @(Get-ChildItem -LiteralPath $runtimeRoot -File -Recurse | ForEach-Object {
        [pscustomobject]@{
            path = $_.FullName.Substring($runtimeRoot.Length + 1).Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    [pscustomobject]@{schema_version=1;version=$runtime.version;archive_sha256=$runtime.sha256;files=$files} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receiptPath -Encoding utf8
}
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
if ($receipt.schema_version -ne 1 -or $receipt.archive_sha256 -ne $runtime.sha256) { throw 'Runtime cache receipt mismatch.' }
$actualFiles = @(Get-ChildItem -LiteralPath $runtimeRoot -File -Recurse)
if ($actualFiles.Count -ne $receipt.files.Count) { throw 'Runtime cache file set mismatch.' }
foreach ($file in $receipt.files) {
    $resolved = [IO.Path]::GetFullPath((Join-Path $runtimeRoot $file.path))
    if (-not $resolved.StartsWith($runtimeRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Runtime receipt contains an invalid path.'
    }
    if ((Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash -ine $file.sha256) { throw 'Runtime cache file hash mismatch.' }
}
if (-not [string]::IsNullOrWhiteSpace($DeployDirectory)) {
    $deploymentRoot = [IO.Path]::GetFullPath($DeployDirectory)
    if (Test-Path -LiteralPath $deploymentRoot) {
        if (@(Get-ChildItem -LiteralPath $deploymentRoot -Force).Count -gt 0) {
            throw 'Terminal runtime deployment requires an empty candidate directory.'
        }
    } else {
        New-Item -ItemType Directory -Path $deploymentRoot -Force | Out-Null
    }
    Get-ChildItem -LiteralPath $runtimeRoot -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $deploymentRoot -Recurse
    }
    foreach ($file in $receipt.files) {
        $copiedPath = Join-Path $deploymentRoot $file.path
        if ((Get-FileHash -LiteralPath $copiedPath -Algorithm SHA256).Hash -ine $file.sha256) {
            throw 'Deployed terminal runtime file hash mismatch.'
        }
    }
    $runtimeRoot = $deploymentRoot
}
# Microsoft requires these read/execute grants for the fixed runtime's renderer
# AppContainer on Windows 10. Scope them to public runtime binaries, never user data.
if ([Environment]::OSVersion.Version.Build -lt 22000) {
    & (Join-Path $env:SystemRoot 'System32/icacls.exe') $runtimeRoot '/grant' '*S-1-15-2-1:(OI)(CI)(RX)' '*S-1-15-2-2:(OI)(CI)(RX)' '/T' '/Q' *> (Join-Path $extractionRoot 'runtime-access.log')
    if ($LASTEXITCODE -ne 0) { throw 'Fixed runtime renderer read/execute access could not be prepared.' }
}
$result = [pscustomobject]@{schema_version=1;status='ready';version=$runtime.version;runtime_directory=$runtimeRoot;file_count=$actualFiles.Count;archive_sha256=$runtime.sha256}
if (-not [string]::IsNullOrWhiteSpace($OutputJsonPath)) { $result | ConvertTo-Json | Set-Content -LiteralPath $OutputJsonPath -Encoding utf8 }
$result | ConvertTo-Json -Compress
