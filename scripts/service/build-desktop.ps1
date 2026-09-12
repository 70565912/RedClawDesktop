<#
.SYNOPSIS
    Configure and build redclaw_desktop (and companion executables).

.PARAMETER Configuration
    Build configuration: Debug (default) or Release.

.PARAMETER SkipConfigure
    Skip the CMake configure step. Use when the build tree is already up to date.

.PARAMETER Target
    Optional CMake target name to build instead of the full graph (e.g. redclaw_desktop).

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
    Number of parallel compile jobs passed to --parallel. Default 8.

.PARAMETER ConfigureTimeoutSeconds
    Maximum configure command duration. 0 disables the watchdog timeout.

.PARAMETER BuildTimeoutSeconds
    Maximum build command duration. 0 disables the watchdog timeout.

.PARAMETER NoOutputTimeoutSeconds
    Maximum time without cmake/ninja output before the guarded wrapper stops
    the process tree. 0 disables the quiet-output watchdog.

.EXAMPLE
    .\build-desktop.ps1
    .\build-desktop.ps1 -Configuration Release
    .\build-desktop.ps1 -SkipConfigure -Target redclaw_tests_unit
#>
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [switch]$SkipConfigure,

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

$scriptRoot   = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot     = Split-Path -Parent (Split-Path -Parent $scriptRoot)
$guardScript  = Join-Path $scriptRoot "run-cmake-guarded.ps1"

Set-Location $repoRoot

if (-not (Test-Path $guardScript)) {
    throw "Guard script not found: $guardScript"
}

function Get-PresetDocument {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PresetPath
    )

    if (-not (Test-Path $PresetPath)) {
        return $null
    }

    return Get-Content $PresetPath -Raw | ConvertFrom-Json
}

function Test-PresetHidden {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Preset
    )

    $hiddenProperty = $Preset.PSObject.Properties['hidden']
    return $null -ne $hiddenProperty -and $hiddenProperty.Value -eq $true
}

function Get-ConfigurePresetNames {
    $names = [System.Collections.Generic.List[string]]::new()
    foreach ($presetFile in @("CMakeUserPresets.json", "CMakePresets.json")) {
        $document = Get-PresetDocument -PresetPath (Join-Path $repoRoot $presetFile)
        if ($null -eq $document -or $null -eq $document.configurePresets) {
            continue
        }

        foreach ($preset in $document.configurePresets) {
            if (Test-PresetHidden -Preset $preset) {
                continue
            }
            if (-not [string]::IsNullOrWhiteSpace($preset.name) -and $names -notcontains $preset.name) {
                $names.Add($preset.name) | Out-Null
            }
        }
    }

    return $names
}

function Get-BuildPresetNames {
    $names = [System.Collections.Generic.List[string]]::new()
    foreach ($presetFile in @("CMakeUserPresets.json", "CMakePresets.json")) {
        $document = Get-PresetDocument -PresetPath (Join-Path $repoRoot $presetFile)
        if ($null -eq $document -or $null -eq $document.buildPresets) {
            continue
        }

        foreach ($preset in $document.buildPresets) {
            if (Test-PresetHidden -Preset $preset) {
                continue
            }
            if (-not [string]::IsNullOrWhiteSpace($preset.name) -and $names -notcontains $preset.name) {
                $names.Add($preset.name) | Out-Null
            }
        }
    }

    return $names
}

function Resolve-PresetName {
    param(
        [string]$Override,
        [string[]]$Candidates,
        [System.Collections.Generic.List[string]]$AvailableNames,
        [string]$PresetKind
    )

    if (-not [string]::IsNullOrWhiteSpace($Override)) {
        return $Override
    }

    foreach ($candidate in $Candidates) {
        if ($AvailableNames -contains $candidate) {
            return $candidate
        }
    }

    throw "No usable $PresetKind preset found. Checked: $($Candidates -join ', ')"
}

$configurePresetNames = Get-ConfigurePresetNames
$buildPresetNames = Get-BuildPresetNames
$resolvedConfigurePreset = Resolve-PresetName `
    -Override $ConfigurePreset `
    -Candidates @("vs2022-x64-local", "ninja-x64-local", "vs2022-x64", "ninja-multi-x64") `
    -AvailableNames $configurePresetNames `
    -PresetKind "configure"
$buildPreset = if ($Configuration -eq "Release") {
    Resolve-PresetName `
        -Override $ReleaseBuildPreset `
        -Candidates @("release-local", "release") `
        -AvailableNames $buildPresetNames `
        -PresetKind "Release build"
} else {
    Resolve-PresetName `
        -Override $DebugBuildPreset `
        -Candidates @("debug-local", "debug") `
        -AvailableNames $buildPresetNames `
        -PresetKind "Debug build"
}
$guardArgsPrefix = @()
if (-not [string]::IsNullOrWhiteSpace($CMakePath)) {
    $guardArgsPrefix += @("-CMakePath", $CMakePath)
}

function New-GuardArgs {
    param([int]$CommandTimeoutSeconds)

    $args = @($guardArgsPrefix)
    if ($CommandTimeoutSeconds -gt 0) {
        $args += @("-CommandTimeoutSeconds", [string]$CommandTimeoutSeconds)
    }
    if ($NoOutputTimeoutSeconds -gt 0) {
        $args += @("-NoOutputTimeoutSeconds", [string]$NoOutputTimeoutSeconds)
    }

    return $args
}

function ConvertTo-CMakeArgsBase64 {
    param([string[]]$ArgumentList)

    $joined = [string]::Join([char]31, @($ArgumentList))
    return [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($joined))
}

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
if (-not $SkipConfigure) {
    $configureArgs = @("--preset", $resolvedConfigurePreset)
    if ($FreshConfigure) {
        $configureArgs = @("--fresh") + $configureArgs
    }

    Write-Host "[build] Configuring: cmake $($configureArgs -join ' ')" -ForegroundColor Cyan
    $configureGuardArgs = New-GuardArgs -CommandTimeoutSeconds $ConfigureTimeoutSeconds
    $configureArgsBase64 = ConvertTo-CMakeArgsBase64 -ArgumentList $configureArgs
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $guardScript @configureGuardArgs -CMakeArgsBase64 $configureArgsBase64
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }
    Write-Host "[build] Configure complete." -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
$buildArgs = @("--build", "--preset", $buildPreset, "--parallel", "$Parallel")
if (-not [string]::IsNullOrWhiteSpace($Target)) {
    $buildArgs += @("--target", $Target)
}

Write-Host "[build] Building: cmake $($buildArgs -join ' ')" -ForegroundColor Cyan
$buildGuardArgs = New-GuardArgs -CommandTimeoutSeconds $BuildTimeoutSeconds
$buildArgsBase64 = ConvertTo-CMakeArgsBase64 -ArgumentList $buildArgs
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $guardScript @buildGuardArgs -CMakeArgsBase64 $buildArgsBase64
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "[build] Build finished successfully." -ForegroundColor Green
Write-Host "[build] Configuration : $Configuration"
$outputRoot = if ($resolvedConfigurePreset -like "ninja*") {
    "$repoRoot\build\ninja-x64\src\$Configuration"
} elseif ($resolvedConfigurePreset -like "vs2022*") {
    "$repoRoot\build\vs2022-x64\src\$Configuration"
} else {
    "$repoRoot\build"
}
Write-Host "[build] Output        : $outputRoot"
Write-Host ""
Write-Host "[build] To publish artifacts run:"
Write-Host "        .\publish-desktop.ps1 -Configuration $Configuration" -ForegroundColor DarkGray
