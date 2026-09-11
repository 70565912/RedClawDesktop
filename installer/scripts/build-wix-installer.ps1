param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$Version = "0.1.0",

    [string]$HostServiceBinaryPath = "",

    [string]$BuildOutputDirectory = "",

    [string]$RuntimePublishDirectory = "",

    [string]$Qt6Dir = ""
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptRoot)
$wixSource = Join-Path $repoRoot "installer/wix/Product.wxs"
$outputDir = Join-Path $repoRoot "build/installer/$Configuration"
$outputMsi = Join-Path $outputDir "RedClawDesktopHost-$Version-$Configuration.msi"
$publishScript = Join-Path $repoRoot "scripts/service/publish-desktop-release.ps1"

function Resolve-RepoPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }

    return (Join-Path $repoRoot $PathValue)
}

function Resolve-OptionalRepoPath {
    param(
        [string]$PathValue
    )

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    return (Resolve-RepoPath $PathValue)
}

$resolvedHostServiceBinaryInput = Resolve-OptionalRepoPath $HostServiceBinaryPath
if (-not [string]::IsNullOrWhiteSpace($resolvedHostServiceBinaryInput) -and (Split-Path -Leaf $resolvedHostServiceBinaryInput) -ne "redclaw_host_service.exe") {
    throw "HostServiceBinaryPath must point to redclaw_host_service.exe: $resolvedHostServiceBinaryInput"
}

$resolvedBuildOutputDirectory = if (-not [string]::IsNullOrWhiteSpace($BuildOutputDirectory)) {
    Resolve-RepoPath $BuildOutputDirectory
} elseif (-not [string]::IsNullOrWhiteSpace($resolvedHostServiceBinaryInput)) {
    Split-Path -Parent $resolvedHostServiceBinaryInput
} else {
    ""
}

$resolvedRuntimePublishDirectory = if ([string]::IsNullOrWhiteSpace($RuntimePublishDirectory)) {
    Join-Path $repoRoot "build/installer/runtime/$Configuration"
} else {
    Resolve-RepoPath $RuntimePublishDirectory
}

if (-not (Test-Path $wixSource)) {
    throw "WiX source file not found: $wixSource"
}

if (-not (Test-Path $publishScript)) {
    throw "Publish helper not found: $publishScript"
}

if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
    throw "WiX CLI not found in PATH. Install WiX Toolset v4 and ensure 'wix' command is available."
}

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$publishParameters = @{
    Configuration = $Configuration
    PublishDirectory = $resolvedRuntimePublishDirectory
}

if (-not [string]::IsNullOrWhiteSpace($resolvedBuildOutputDirectory)) {
    $publishParameters.BuildOutputDirectory = $resolvedBuildOutputDirectory
}

if (-not [string]::IsNullOrWhiteSpace($Qt6Dir)) {
    $publishParameters.Qt6Dir = $Qt6Dir
}

$publishArgumentDisplay = $publishParameters.GetEnumerator() |
    Sort-Object Name |
    ForEach-Object { "-$($_.Name) $($_.Value)" }

Write-Host "Staging runtime publish directory via: $publishScript $($publishArgumentDisplay -join ' ')"
& $publishScript @publishParameters

if ($LASTEXITCODE -ne 0) {
    throw "Runtime publish staging failed with exit code $LASTEXITCODE"
}

$hostServiceBinary = Join-Path $resolvedRuntimePublishDirectory "redclaw_host_service.exe"
if (-not (Test-Path $hostServiceBinary)) {
    throw "Published host service binary not found: $hostServiceBinary"
}

$arguments = @(
    "build",
    $wixSource,
    "-arch", "x64",
    "-d", "HostServiceBinaryPath=$hostServiceBinary",
    "-d", "RuntimePublishDirectory=$resolvedRuntimePublishDirectory",
    "-o", $outputMsi
)

Write-Host "Building installer with command: wix $($arguments -join ' ')"
&wix @arguments

if ($LASTEXITCODE -ne 0) {
    throw "WiX build failed with exit code $LASTEXITCODE"
}

Write-Host "Installer generated: $outputMsi"
