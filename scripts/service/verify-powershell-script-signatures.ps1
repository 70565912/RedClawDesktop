param(
    [string]$ScriptsRoot = "scripts",
    [switch]$AllowUnsigned
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$resolvedScriptsRoot = Join-Path $repoRoot $ScriptsRoot
if (-not (Test-Path $resolvedScriptsRoot)) {
    throw "Scripts root not found: $resolvedScriptsRoot"
}

$scripts = Get-ChildItem -Path $resolvedScriptsRoot -Recurse -File -Filter "*.ps1"
if ($scripts.Count -eq 0) {
    Write-Host "No scripts found under $resolvedScriptsRoot"
    exit 0
}

$issues = @()
foreach ($script in $scripts) {
    $signature = Get-AuthenticodeSignature -FilePath $script.FullName
    if ($signature.Status -eq "Valid") {
        continue
    }

    if ($AllowUnsigned -and $signature.Status -eq "NotSigned") {
        continue
    }

    $issues += [pscustomobject]@{
        Script = $script.FullName
        Status = $signature.Status
        Message = $signature.StatusMessage
    }
}

if ($issues.Count -gt 0) {
    $issues | Format-Table -AutoSize | Out-String | Write-Host
    throw "Script signature verification failed."
}

Write-Host "All script signatures are valid."
