<#
.SYNOPSIS
    Publish redclaw_desktop build artifacts to the release directory.

.DESCRIPTION
    Copies the CMake build output for the selected configuration into
    release\<Configuration>\ and deploys Qt runtime DLLs via windeployqt.
    The destination directory is wiped before each publish to guarantee a
    clean layout.

.PARAMETER Configuration
    Build configuration to publish: Debug (default) or Release.

.PARAMETER BuildOutputDirectory
    Path to the directory containing the compiled binaries.  If omitted the
    script searches the standard build tree locations.

.PARAMETER PublishDirectory
    Destination directory.  Defaults to <repo>\release\<Configuration>.

.PARAMETER Qt6Dir
    Path to the Qt6 cmake directory (e.g. C:\Qt\6.7.2\msvc2022_64\lib\cmake\Qt6).
    Falls back to the Qt6_DIR environment variable and the CMake cache.

.EXAMPLE
    .\publish-desktop.ps1
    .\publish-desktop.ps1 -Configuration Release
    .\publish-desktop.ps1 -Configuration Debug -PublishDirectory C:\Staging\redclaw
#>
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$BuildOutputDirectory = "",

    [string]$PublishDirectory = "",

    [string]$Qt6Dir = ""
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot   = Split-Path -Parent (Split-Path -Parent $scriptRoot)

# ---------------------------------------------------------------------------
# Helper: resolve a path relative to the repo root when not absolute
# ---------------------------------------------------------------------------
function Resolve-RepoPath {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }
    return (Join-Path $repoRoot $PathValue)
}

# ---------------------------------------------------------------------------
# Locate the build output directory
# ---------------------------------------------------------------------------
function Get-BuildOutputDir {
    param([string]$Configuration, [string]$Override)

    if (-not [string]::IsNullOrWhiteSpace($Override)) {
        $resolved = Resolve-RepoPath $Override
        if (-not (Test-Path $resolved)) {
            throw "Build output directory not found: $resolved"
        }
        return $resolved
    }

    $candidates = @(
        (Join-Path $repoRoot "build/vs2022-x64/src/$Configuration"),
        (Join-Path $repoRoot "build/ninja-x64/src/$Configuration"),
        (Join-Path $repoRoot "build/ci-installer/src/$Configuration")
    )

    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($found)) {
        throw "Build output directory not found. Searched:`n  $($candidates -join "`n  ")`nRun build-desktop.ps1 first."
    }
    return $found
}

# ---------------------------------------------------------------------------
# Locate Qt6 cmake directory from CMake cache
# ---------------------------------------------------------------------------
function Get-Qt6DirFromCache {
    $caches = @(
        (Join-Path $repoRoot "build/vs2022-x64/CMakeCache.txt"),
        (Join-Path $repoRoot "build/ninja-x64/CMakeCache.txt")
    )
    foreach ($cache in $caches) {
        if (-not (Test-Path $cache)) { continue }
        $match = Select-String -Path $cache -Pattern '^Qt6_DIR(?::[^=]+)?=(?<P>.+)$' |
                 Select-Object -First 1
        if ($null -eq $match) { continue }
        $val = $match.Matches[0].Groups['P'].Value.Trim()
        if (-not [string]::IsNullOrWhiteSpace($val) -and $val -ne "Qt6_DIR-NOTFOUND" -and (Test-Path $val)) {
            return $val
        }
    }
    return ""
}

# ---------------------------------------------------------------------------
# Locate windeployqt.exe
# ---------------------------------------------------------------------------
function Get-WinDeployQt {
    param([string]$Qt6DirHint)

    $searchDirs = [System.Collections.Generic.List[string]]::new()

    foreach ($candidate in @($Qt6DirHint, $env:Qt6_DIR, (Get-Qt6DirFromCache))) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            $searchDirs.Add($candidate) | Out-Null
        }
    }

    foreach ($dir in $searchDirs) {
        $resolved = Resolve-RepoPath $dir
        if (-not (Test-Path $resolved)) { continue }
        $qtRoot = (Resolve-Path (Join-Path $resolved "../../..")).Path
        $tool   = Join-Path $qtRoot "bin/windeployqt.exe"
        if (Test-Path $tool) {
            return @{ Path = $tool; QtRoot = $qtRoot }
        }
    }

    $cmd = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($null -ne $cmd) {
        return @{ Path = $cmd.Source; QtRoot = (Split-Path -Parent (Split-Path -Parent $cmd.Source)) }
    }

    return $null
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
$sourceDir = Get-BuildOutputDir -Configuration $Configuration -Override $BuildOutputDirectory
$terminalRequirement = Join-Path $sourceDir 'terminal-runtime.json'
$terminalRequired = Test-Path -LiteralPath $terminalRequirement
if ($terminalRequired) {
    $builtTerminal = Get-Content -LiteralPath $terminalRequirement -Raw | ConvertFrom-Json
    $sourceTerminal = Get-Content -LiteralPath (Join-Path $repoRoot 'third_party/terminal/manifest.json') -Raw | ConvertFrom-Json
    if ($builtTerminal.schema_version -ne 1 -or $builtTerminal.runtime.sha256 -ne $sourceTerminal.runtime.sha256) {
        throw 'Terminal dependency version differs from the built program; rebuild before publishing.'
    }
    # Download and validate before replacing any running-directory contents.
    & (Join-Path $scriptRoot 'prepare-terminal-runtime.ps1') | Out-Null
}

$destDir = if ([string]::IsNullOrWhiteSpace($PublishDirectory)) {
    Join-Path $repoRoot "release\$Configuration"
} else {
    Resolve-RepoPath $PublishDirectory
}

# Guard: never wipe the repo root
$resolvedRepo = (Resolve-Path $repoRoot).Path
New-Item -ItemType Directory -Force -Path $destDir | Out-Null
$resolvedDest = (Resolve-Path $destDir).Path
if ($resolvedDest -eq $resolvedRepo) {
    throw "Publish directory must not be the repository root: $resolvedDest"
}

Write-Host "[publish] Source      : $sourceDir"
Write-Host "[publish] Destination : $resolvedDest"
Write-Host "[publish] Clearing destination..."
Get-ChildItem -Path $resolvedDest -Force -ErrorAction SilentlyContinue |
    Remove-Item -Force -Recurse

Write-Host "[publish] Copying build output..."
Copy-Item -Path (Join-Path $sourceDir '*') -Destination $resolvedDest -Force -Recurse

$exePath = Join-Path $resolvedDest "redclaw_desktop.exe"
if (-not (Test-Path $exePath)) {
    throw "redclaw_desktop.exe was not found in the publish directory after copy: $resolvedDest"
}

# ---------------------------------------------------------------------------
# Qt runtime deployment
# ---------------------------------------------------------------------------
$qtIsConfigured = (-not [string]::IsNullOrWhiteSpace($Qt6Dir)) -or
                  (-not [string]::IsNullOrWhiteSpace($env:Qt6_DIR)) -or
                  (-not [string]::IsNullOrWhiteSpace((Get-Qt6DirFromCache)))

$deployTool = Get-WinDeployQt -Qt6DirHint $Qt6Dir

if ($null -eq $deployTool) {
    if ($qtIsConfigured) {
        throw "Qt is configured for this build but windeployqt.exe was not found.`nSet Qt6_DIR or add the Qt bin directory to PATH."
    }
    Write-Host "[publish] windeployqt not found; skipping Qt runtime deployment." -ForegroundColor Yellow
} else {
    $deployArgs = @(
        "--dir", $resolvedDest,
        "--force",
        "--no-translations",
        "--compiler-runtime"
    )
    if ($Configuration -eq "Debug") {
        $deployArgs += "--debug"
    } else {
        $deployArgs += "--release"
    }
    $deployArgs += $exePath

    Write-Host "[publish] Deploying Qt runtime: $($deployTool.Path)"
    & $deployTool.Path @deployArgs
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed with exit code $LASTEXITCODE"
    }

    $platformPlugin = Get-ChildItem -Path (Join-Path $resolvedDest "platforms") `
        -Filter "qwindows*.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $platformPlugin) {
        throw "Qt deployment completed but platforms\qwindows*.dll is missing from: $resolvedDest"
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
if ($terminalRequired) {
    Write-Host '[publish] Deploying verified fixed terminal runtime...'
    & (Join-Path $scriptRoot 'prepare-terminal-runtime.ps1') -DeployDirectory (Join-Path $resolvedDest 'terminal-runtime') | Out-Null
    $maintenanceDirectory = Join-Path $resolvedDest 'maintenance'
    New-Item -ItemType Directory -Path $maintenanceDirectory -Force | Out-Null
    foreach ($name in @('terminal-profile.ps1','start-runtime-maintenance.ps1','runtime-upgrade-common.ps1','invoke-runtime-directory-upgrade.ps1','invoke-workspace-control.ps1')) {
        Copy-Item -LiteralPath (Join-Path $scriptRoot $name) -Destination (Join-Path $maintenanceDirectory $name)
    }
}
$files = (Get-ChildItem -Path $resolvedDest -File -Recurse).Count
Write-Host ""
Write-Host "[publish] Done. $files file(s) in: $resolvedDest" -ForegroundColor Green
Write-Host "[publish] Run with:"
Write-Host "          & '$resolvedDest\redclaw_desktop.exe'" -ForegroundColor DarkGray
