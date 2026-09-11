<#
.SYNOPSIS
    Configure, build and publish redclaw_desktop to release\<Configuration>\.

.DESCRIPTION
    Runs cmake configure (unless -SkipConfigure), cmake build, then publishes
    the output to release\<Configuration>\ via publish.ps1.

    This is the standard local dev workflow:
        .\build.ps1                      # Debug build → release\Debug\
        .\build.ps1 -Configuration Release   # Release build → release\Release\
        .\build.ps1 -SkipConfigure           # Skip configure, re-build + publish
        .\build.ps1 -NoPublish               # Build only, skip publish step
        .\build.ps1 -Target redclaw_tests_unit -NoPublish

.PARAMETER Configuration
    Debug (default) or Release.

.PARAMETER SkipConfigure
    Skip cmake configure step when the build tree is already up to date.

.PARAMETER NoPublish
    Skip the publish step. Build output stays in the CMake build tree only.

.PARAMETER Target
    Optional CMake target (e.g. redclaw_tests_unit). Builds everything when omitted.
    When a specific target is given, publish is skipped automatically unless the
    target is redclaw_desktop.

.PARAMETER FreshConfigure
    Run CMake configure with --fresh. Useful after moving toolchains, vcpkg, Qt,
    or Visual Studio to a different path.

.PARAMETER CMakePath
    Optional explicit cmake.exe path. When omitted, the guarded wrapper searches
    PATH, Program Files CMake, and Visual Studio bundled CMake.

.PARAMETER ConfigurePreset
    Configure preset to use. Defaults to auto-selecting a non-hidden local preset.

.PARAMETER DebugBuildPreset
    Build preset for Debug configuration. Defaults to auto-selecting debug-local or debug.

.PARAMETER ReleaseBuildPreset
    Build preset for Release configuration. Defaults to auto-selecting release-local or release.

.PARAMETER Parallel
    Parallel compile jobs. Default 8.

.PARAMETER ConfigureTimeoutSeconds
    Maximum configure command duration. 0 disables the watchdog timeout.

.PARAMETER BuildTimeoutSeconds
    Maximum build command duration. 0 disables the watchdog timeout.

.PARAMETER NoOutputTimeoutSeconds
    Maximum time without cmake/ninja output before the guarded wrapper stops
    the process tree. 0 disables the quiet-output watchdog.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Configuration Release
    .\build.ps1 -SkipConfigure -Target redclaw_desktop
#>
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [switch]$SkipConfigure,

    [switch]$NoPublish,

    [string]$Target = "",

    [switch]$FreshConfigure,

    [string]$CMakePath = "",

    [string]$ConfigurePreset = "",

    [string]$DebugBuildPreset = "",

    [string]$ReleaseBuildPreset = "",

    [int]$Parallel = 8,

    [ValidateRange(0, 86400)]
    [int]$ConfigureTimeoutSeconds = 3600,

    [ValidateRange(0, 86400)]
    [int]$BuildTimeoutSeconds = 1800,

    [ValidateRange(0, 86400)]
    [int]$NoOutputTimeoutSeconds = 900
)

$ErrorActionPreference = "Stop"

$buildImpl   = Join-Path $PSScriptRoot "scripts\service\build-desktop.ps1"
$publishImpl = Join-Path $PSScriptRoot "publish.ps1"

if (-not (Test-Path $buildImpl)) {
    throw "Build script not found: $buildImpl"
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
$buildArgs = @{
    Configuration = $Configuration
    Parallel = $Parallel
    ConfigurePreset = $ConfigurePreset
    DebugBuildPreset = $DebugBuildPreset
    ReleaseBuildPreset = $ReleaseBuildPreset
    ConfigureTimeoutSeconds = $ConfigureTimeoutSeconds
    BuildTimeoutSeconds = $BuildTimeoutSeconds
    NoOutputTimeoutSeconds = $NoOutputTimeoutSeconds
}
if ($SkipConfigure)                              { $buildArgs['SkipConfigure']  = $true }
if ($FreshConfigure)                             { $buildArgs['FreshConfigure'] = $true }
if (-not [string]::IsNullOrWhiteSpace($Target)) { $buildArgs['Target']         = $Target }
if (-not [string]::IsNullOrWhiteSpace($CMakePath)) { $buildArgs['CMakePath']    = $CMakePath }

& $buildImpl @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---------------------------------------------------------------------------
# Publish  (skip when -NoPublish, or when building a non-desktop target)
# ---------------------------------------------------------------------------
$isDesktopTarget = [string]::IsNullOrWhiteSpace($Target) -or $Target -eq "redclaw_desktop"
if ($NoPublish -or -not $isDesktopTarget) {
    if (-not $isDesktopTarget) {
        Write-Host "[build] Skipping publish (target: $Target is not redclaw_desktop)." -ForegroundColor DarkGray
    }
    exit 0
}

if (-not (Test-Path $publishImpl)) {
    Write-Host "[build] Warning: publish.ps1 not found at $publishImpl; skipping publish." -ForegroundColor Yellow
    exit 0
}

Write-Host ""
Write-Host "[build] Publishing artifacts..." -ForegroundColor Cyan
& $publishImpl -Configuration $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
