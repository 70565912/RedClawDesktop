param(
    [Parameter(Mandatory = $true)]
    [string]$DestinationDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$requiredRuntimeDlls = @(
    'msvcp140.dll',
    'msvcp140_1.dll',
    'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll',
    'vcruntime140.dll',
    'vcruntime140_1.dll'
)

if (-not (Test-Path -LiteralPath $DestinationDirectory -PathType Container)) {
    throw "MSVC runtime destination is missing: $DestinationDirectory"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe is missing: $vswhere"
}

$installation = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installation)) {
    throw 'Visual Studio C++ toolset was not found; the desktop package cannot include its runtime.'
}

$redistRoot = Join-Path $installation.Trim() 'VC\Redist\MSVC'
$candidates = @(Get-ChildItem -LiteralPath $redistRoot -Recurse -Filter 'msvcp140_atomic_wait.dll' -File -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Directory.Name -match '^Microsoft\.VC\d+\.CRT$' -and
        $_.Directory.Parent.Name -eq 'x64' -and
        $_.Directory.Parent.Parent.Name -match '^\d+\.\d+\.\d+$'
    })
if ($candidates.Count -eq 0) {
    throw "MSVC desktop CRT was not found under $redistRoot"
}

$sourceDirectory = ($candidates |
    Sort-Object { [version]$_.Directory.Parent.Parent.Name } -Descending |
    Select-Object -First 1).DirectoryName

foreach ($runtimeDll in $requiredRuntimeDlls) {
    $source = Join-Path $sourceDirectory $runtimeDll
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required MSVC runtime DLL is missing from ${sourceDirectory}: $runtimeDll"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $DestinationDirectory $runtimeDll) -Force
}

Write-Host "[publish] Copied MSVC desktop runtime from $sourceDirectory"
