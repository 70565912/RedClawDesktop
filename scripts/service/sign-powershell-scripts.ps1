param(
    [Parameter(Mandatory = $true)]
    [string]$CertificateThumbprint,
    [string]$ScriptsRoot = "scripts",
    [string]$TimestampServer = "http://timestamp.digicert.com",
    [switch]$UseLocalMachineStore
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$resolvedScriptsRoot = Join-Path $repoRoot $ScriptsRoot
if (-not (Test-Path $resolvedScriptsRoot)) {
    throw "Scripts root not found: $resolvedScriptsRoot"
}

$storePath = if ($UseLocalMachineStore) { "Cert:\LocalMachine\My" } else { "Cert:\CurrentUser\My" }
$certificate = Get-ChildItem -Path $storePath |
    Where-Object { $_.Thumbprint -eq $CertificateThumbprint } |
    Select-Object -First 1

if ($null -eq $certificate) {
    throw "Code-signing certificate not found in $storePath for thumbprint: $CertificateThumbprint"
}

$scripts = Get-ChildItem -Path $resolvedScriptsRoot -Recurse -File -Filter "*.ps1"
if ($scripts.Count -eq 0) {
    Write-Host "No scripts found under $resolvedScriptsRoot"
    exit 0
}

$failed = @()
foreach ($script in $scripts) {
    $signature = Set-AuthenticodeSignature -FilePath $script.FullName -Certificate $certificate -TimestampServer $TimestampServer
    if ($signature.Status -ne "Valid") {
        $failed += [pscustomobject]@{
            Script = $script.FullName
            Status = $signature.Status
            Message = $signature.StatusMessage
        }
    }
}

if ($failed.Count -gt 0) {
    $failed | Format-Table -AutoSize | Out-String | Write-Host
    throw "Some scripts failed to sign."
}

Write-Host "Signed $($scripts.Count) script(s) successfully using certificate $CertificateThumbprint."
