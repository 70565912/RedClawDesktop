param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$PublishDirectory = "",

    [string]$BuildOutputDirectory = "",

    [string]$Qt6Dir = ""
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

    return (Join-Path $repoRoot $PathValue)
}

function Get-BuildOutputDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Configuration,

        [string]$BuildOutputDirectory
    )

    if (-not [string]::IsNullOrWhiteSpace($BuildOutputDirectory)) {
        $resolvedPath = Resolve-RepoPath $BuildOutputDirectory
        if (-not (Test-Path $resolvedPath)) {
            throw "Build output directory not found: $resolvedPath"
        }

        return $resolvedPath
    }

    $defaultCandidates = @(
        (Join-Path $repoRoot "build/ci-installer/src/$Configuration"),
        (Join-Path $repoRoot "build/vs2022-x64/src/$Configuration"),
        (Join-Path $repoRoot "build/ninja-x64/src/$Configuration")
    )

    $resolvedCandidate = $defaultCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($resolvedCandidate)) {
        throw "Build output directory not found. Expected one of: $($defaultCandidates -join ', ')"
    }

    return $resolvedCandidate
}

function Get-Qt6DirFromCMakeCache {
    $cacheCandidates = @(
        (Join-Path $repoRoot "build/vs2022-x64/CMakeCache.txt"),
        (Join-Path $repoRoot "build/ninja-x64/CMakeCache.txt")
    )

    foreach ($cachePath in $cacheCandidates) {
        if (-not (Test-Path $cachePath)) {
            continue
        }

        $match = Select-String -Path $cachePath -Pattern '^Qt6_DIR(?::[^=]+)?=(?<Path>.+)$' | Select-Object -First 1
        if ($null -eq $match) {
            continue
        }

        $candidatePath = $match.Matches[0].Groups['Path'].Value.Trim()
        if ([string]::IsNullOrWhiteSpace($candidatePath) -or $candidatePath -eq "Qt6_DIR-NOTFOUND") {
            continue
        }

        if (Test-Path $candidatePath) {
            return $candidatePath
        }
    }

    return ""
}

function Get-QtDeployTool {
    param(
        [string]$Qt6Dir
    )

    $candidateDirs = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($Qt6Dir)) {
        $candidateDirs.Add((Resolve-RepoPath $Qt6Dir)) | Out-Null
    }
    if (-not [string]::IsNullOrWhiteSpace($env:Qt6_DIR)) {
        $candidateDirs.Add($env:Qt6_DIR) | Out-Null
    }

    $cachedQt6Dir = Get-Qt6DirFromCMakeCache
    if (-not [string]::IsNullOrWhiteSpace($cachedQt6Dir)) {
        $candidateDirs.Add($cachedQt6Dir) | Out-Null
    }

    foreach ($candidateDir in $candidateDirs) {
        if (-not (Test-Path $candidateDir)) {
            continue
        }

        $qtRoot = (Resolve-Path (Join-Path $candidateDir "..\..\.."))
        $windeployqtPath = Join-Path $qtRoot.Path "bin/windeployqt.exe"
        if (Test-Path $windeployqtPath) {
            return @{
                Path = $windeployqtPath
                Qt6Dir = $candidateDir
                QtRoot = $qtRoot.Path
            }
        }
    }

    $command = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        $qtBinDir = Split-Path -Parent $command.Source
        return @{
            Path = $command.Source
            Qt6Dir = ""
            QtRoot = (Split-Path -Parent $qtBinDir)
        }
    }

    return $null
}

$sourceDirectory = Get-BuildOutputDirectory -Configuration $Configuration -BuildOutputDirectory $BuildOutputDirectory
$publishDirectory = if ([string]::IsNullOrWhiteSpace($PublishDirectory)) {
    Join-Path $repoRoot (Join-Path "release" $Configuration)
} else {
    Resolve-RepoPath $PublishDirectory
}

New-Item -ItemType Directory -Force -Path $publishDirectory | Out-Null

$resolvedRepoRoot = (Resolve-Path $repoRoot).Path
$resolvedPublishDirectory = (Resolve-Path $publishDirectory).Path
if ($resolvedPublishDirectory -eq $resolvedRepoRoot) {
    throw "Publish directory cannot be the repository root: $resolvedPublishDirectory"
}

Write-Host "[publish] source: $sourceDirectory"
Write-Host "[publish] output: $resolvedPublishDirectory"

Get-ChildItem -Path $resolvedPublishDirectory -Force -ErrorAction SilentlyContinue | Remove-Item -Force -Recurse
Copy-Item -Path (Join-Path $sourceDirectory '*') -Destination $resolvedPublishDirectory -Force -Recurse

$desktopExecutable = Join-Path $resolvedPublishDirectory "redclaw_desktop.exe"
if (-not (Test-Path $desktopExecutable)) {
    throw "Desktop executable was not copied to publish directory: $desktopExecutable"
}

$qtDeployTool = Get-QtDeployTool -Qt6Dir $Qt6Dir
$qtConfigured = -not [string]::IsNullOrWhiteSpace($Qt6Dir) -or -not [string]::IsNullOrWhiteSpace($env:Qt6_DIR) -or -not [string]::IsNullOrWhiteSpace((Get-Qt6DirFromCMakeCache))

if ($null -eq $qtDeployTool) {
    if ($qtConfigured) {
        throw "Qt6 is configured for this build, but windeployqt.exe could not be found. Set Qt6_DIR or add the Qt bin directory to PATH."
    }

    Write-Host "[publish] Qt deploy tool not found; skipping Qt runtime deployment." -ForegroundColor Yellow
}
else {
    $deployArguments = @(
        "--dir", $resolvedPublishDirectory,
        "--force",
        "--no-translations",
        "--compiler-runtime"
    )

    if ($Configuration -eq "Debug") {
        $deployArguments += "--debug"
    }
    else {
        $deployArguments += "--release"
    }

    $deployArguments += $desktopExecutable

    Write-Host "[publish] Deploying Qt runtime via: $($qtDeployTool.Path)"
    & $qtDeployTool.Path @deployArguments
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed with exit code $LASTEXITCODE"
    }

    $platformPlugin = Get-ChildItem -Path (Join-Path $resolvedPublishDirectory "platforms") -Filter "qwindows*.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $platformPlugin) {
        throw "Qt deployment completed, but platforms/qwindows plugin is missing from $resolvedPublishDirectory"
    }
}

Write-Host "[publish] Published runtime directory is ready: $resolvedPublishDirectory"