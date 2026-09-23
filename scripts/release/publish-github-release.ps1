param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [switch]$PackageOnly,

    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptRoot)
$tagName = "v$Version"
$releaseName = "RedClawDesktop-windows-x64-$tagName"
$releaseDirectory = Join-Path $repoRoot 'release\Release'
$distDirectory = Join-Path $repoRoot 'release\dist'
$stagingRoot = Join-Path $distDirectory "staging-$tagName"
$stagingDirectory = Join-Path $stagingRoot 'RedClawDesktop'
$zipPath = Join-Path $distDirectory "$releaseName.zip"
$checksumPath = Join-Path $distDirectory 'SHA256SUMS.txt'
$releaseNotesPath = Join-Path $repoRoot "docs\releases\$tagName.md"

function Assert-LastExitCode {
    param([Parameter(Mandatory = $true)][string]$Name)
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

function Invoke-CapturedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList
    )

    # Windows PowerShell treats native stderr as a terminating error when
    # ErrorActionPreference is Stop. A missing release is an expected probe.
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $FilePath @ArgumentList 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    $text = @(foreach ($line in @($output)) { "$line" }) -join [Environment]::NewLine
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $text.Trim()
    }
}

function Assert-Version {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') {
        throw "Version must use MAJOR.MINOR.PATCH without a leading v: $Version"
    }

    $cmake = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
    $match = [regex]::Match($cmake, '(?ms)project\s*\(\s*RedClawDesktop\s+VERSION\s+(?<version>\d+\.\d+\.\d+)')
    if (-not $match.Success) {
        throw 'Unable to read the RedClawDesktop version from CMakeLists.txt.'
    }
    if ($match.Groups['version'].Value -ne $Version) {
        throw "CMake version $($match.Groups['version'].Value) does not match requested release $Version."
    }

    if (-not (Test-Path -LiteralPath $releaseNotesPath -PathType Leaf)) {
        throw "Release notes are missing: $releaseNotesPath"
    }
}

function Assert-PublishPreflight {
    Push-Location $repoRoot
    try {
        $branch = (& git branch --show-current).Trim()
        Assert-LastExitCode -Name 'git branch --show-current'
        if ($branch -ne 'main') {
            throw "GitHub releases must be published from main; current branch is $branch."
        }

        $status = @(& git status --porcelain --untracked-files=all)
        Assert-LastExitCode -Name 'git status'
        if ($status.Count -gt 0) {
            throw "The release worktree is not clean:`n$($status -join "`n")"
        }

        $remote = (& git remote get-url origin).Trim()
        Assert-LastExitCode -Name 'git remote get-url origin'
        if ($remote -notmatch 'github\.com[:/]70565912/RedClawDesktop(?:\.git)?$') {
            throw "origin is not the public RedClawDesktop repository: $remote"
        }

        $auth = Invoke-CapturedCommand -FilePath 'gh' -ArgumentList @('api', 'user', '--jq', '.login')
        if ($auth.ExitCode -ne 0 -or $auth.Output -ne '70565912') {
            throw 'GitHub CLI is not authenticated as 70565912.'
        }

        $localTag = Invoke-CapturedCommand -FilePath 'git' -ArgumentList @('rev-parse', '-q', '--verify', "refs/tags/$tagName")
        if ($localTag.ExitCode -eq 0) {
            throw "Local tag already exists: $tagName"
        }

        $remoteTag = Invoke-CapturedCommand -FilePath 'git' -ArgumentList @('ls-remote', '--tags', 'origin', "refs/tags/$tagName")
        if ($remoteTag.ExitCode -ne 0) {
            throw "Unable to query remote tag $tagName."
        }
        if (-not [string]::IsNullOrWhiteSpace($remoteTag.Output)) {
            throw "Remote tag already exists: $tagName"
        }

        $release = Invoke-CapturedCommand -FilePath 'gh' -ArgumentList @('release', 'view', $tagName)
        if ($release.ExitCode -eq 0) {
            throw "GitHub Release already exists: $tagName"
        }
    } finally {
        Pop-Location
    }
}

function Copy-ReleaseRuntime {
    if (-not (Test-Path -LiteralPath $releaseDirectory -PathType Container)) {
        throw "Release publish directory is missing: $releaseDirectory"
    }

    $desktop = Join-Path $releaseDirectory 'redclaw_desktop.exe'
    if (-not (Test-Path -LiteralPath $desktop -PathType Leaf)) {
        throw "Release desktop executable is missing: $desktop"
    }

    New-Item -ItemType Directory -Force -Path $stagingDirectory | Out-Null
    Copy-Item -LiteralPath $desktop -Destination $stagingDirectory -Force
    $codecTool = Join-Path $releaseDirectory 'redclaw_protocol_codec.exe'
    if (-not (Test-Path -LiteralPath $codecTool -PathType Leaf)) { throw 'Release protocol codec tool is missing.' }
    Copy-Item -LiteralPath $codecTool -Destination $stagingDirectory -Force

    foreach ($dll in @(Get-ChildItem -LiteralPath $releaseDirectory -File -Filter '*.dll')) {
        Copy-Item -LiteralPath $dll.FullName -Destination $stagingDirectory -Force
    }

    foreach ($pluginDirectoryName in @(
        'iconengines',
        'imageformats',
        'networkinformation',
        'platforms',
        'styles',
        'tls'
    )) {
        $source = Join-Path $releaseDirectory $pluginDirectoryName
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            continue
        }

        $destination = Join-Path $stagingDirectory $pluginDirectoryName
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        foreach ($dll in @(Get-ChildItem -LiteralPath $source -File -Filter '*.dll')) {
            Copy-Item -LiteralPath $dll.FullName -Destination $destination -Force
        }
    }

    if (Test-Path -LiteralPath (Join-Path $releaseDirectory 'terminal-runtime.json')) {
        foreach ($required in @('terminal-runtime/msedgewebview2.exe',
            'maintenance/terminal-profile.ps1', 'maintenance/start-runtime-maintenance.ps1',
            'maintenance/runtime-upgrade-common.ps1', 'maintenance/invoke-runtime-directory-upgrade.ps1')) {
            if (-not (Test-Path -LiteralPath (Join-Path $releaseDirectory $required) -PathType Leaf)) {
                throw "Terminal release bundle is incomplete: $required"
            }
        }
        Copy-Item -LiteralPath (Join-Path $releaseDirectory 'terminal-runtime.json') -Destination $stagingDirectory
        foreach ($required in @('terminal-notices/xterm-LICENSE.txt', 'terminal-notices/fit-LICENSE.txt',
            'terminal-notices/WebView2-SDK-LICENSE.txt', 'terminal-notices/WebView2-SDK-NOTICE.txt')) {
            if (-not (Test-Path -LiteralPath (Join-Path $releaseDirectory $required) -PathType Leaf)) {
                throw "Terminal release notice is missing: $required"
            }
        }
        foreach ($directory in @('terminal-runtime', 'maintenance', 'terminal-notices')) {
            Copy-Item -LiteralPath (Join-Path $releaseDirectory $directory) -Destination $stagingDirectory -Recurse
        }
    }

    Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') -Destination $stagingDirectory -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot 'README.en.md') -Destination $stagingDirectory -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination $stagingDirectory -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.md') -Destination $stagingDirectory -Force
    Copy-Item -LiteralPath $releaseNotesPath -Destination (Join-Path $stagingDirectory 'RELEASE-NOTES.md') -Force
    $licenseDestination = Join-Path $stagingDirectory 'third_party\licenses'
    New-Item -ItemType Directory -Force -Path $licenseDestination | Out-Null
    Copy-Item -Path (Join-Path $repoRoot 'third_party\licenses\*') `
        -Destination $licenseDestination -Recurse -Force
}

function Get-RelativeStagingPath {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$FullName
    )
    # Path.GetRelativePath is unavailable on Windows PowerShell 5 / .NET Framework.
    $base = (Resolve-Path -LiteralPath $BaseDirectory).Path.TrimEnd('\') + '\'
    $relative = [Uri]::UnescapeDataString(([Uri]$base).MakeRelativeUri([Uri]$FullName).ToString())
    return $relative.Replace('\', '/')
}

function Get-StagedRelativeFiles {
    return @(
        Get-ChildItem -LiteralPath $stagingDirectory -Recurse -File |
            ForEach-Object { Get-RelativeStagingPath -BaseDirectory $stagingDirectory -FullName $_.FullName } |
            Sort-Object
    )
}

function Assert-StagingContent {
    $files = Get-StagedRelativeFiles
    if ($files -notcontains 'redclaw_desktop.exe') {
        throw 'Staging validation failed: redclaw_desktop.exe is missing.'
    }
    if ($files -notcontains 'redclaw_protocol_codec.exe') { throw 'Staging validation failed: protocol codec is missing.' }
    if (-not ($files | Where-Object { $_ -eq 'platforms/qwindows.dll' })) {
        throw 'Staging validation failed: platforms/qwindows.dll is missing.'
    }

    $violations = @($files | Where-Object {
        $_ -match '(?i)(^|/)(redclaw_host_service|redclaw_debug_bridge)\.exe$' -or
        $_ -match '(?i)\.(lib|pdb|exp|ilk|obj|log|db|sqlite|sqlite3|conf|pem|key)$' -or
        $_ -match '(?i)(^|/)(reports?|runtime-signaling|logs?|cache|credentials?)(/|$)' -or
        $_ -match '(?i)(^|/)(Qt6[^/]*d|libprotobufd|qsvgicond|qgifd|qicod|qjpegd|qsvgd|qnetworklistmanagerd|qwindowsd|qwindowsvistastyled|qcertonlybackendd|qopensslbackendd|qschannelbackendd)\.dll$' -or
        $_ -match '(?i)gd-x64-[^/]+\.dll$'
    })
    if ($violations.Count -gt 0) {
        throw "Staging validation failed; forbidden files found:`n$($violations -join "`n")"
    }
}

function Write-InnerManifest {
    Push-Location $repoRoot
    try {
        $gitSha = (& git rev-parse HEAD).Trim()
        Assert-LastExitCode -Name 'git rev-parse HEAD'
    } finally {
        Pop-Location
    }

    $exePath = Join-Path $stagingDirectory 'redclaw_desktop.exe'
    $exeHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $files = Get-StagedRelativeFiles
    $lines = @(
        'RedClawDesktop release manifest',
        "version=$Version",
        "tag=$tagName",
        "git_sha=$gitSha",
        "redclaw_desktop_sha256=$exeHash",
        'zip_sha256=See the separately published SHA256SUMS.txt file.',
        '',
        'files:'
    ) + @($files | ForEach-Object { "- $_" })
    [System.IO.File]::WriteAllLines(
        (Join-Path $stagingDirectory 'RELEASE-MANIFEST.txt'),
        $lines,
        [System.Text.UTF8Encoding]::new($false))

    return [pscustomobject]@{ GitSha = $gitSha; ExeSha256 = $exeHash }
}

function New-PortableArchive {
    New-Item -ItemType Directory -Force -Path $distDirectory | Out-Null
    $resolvedDist = (Resolve-Path -LiteralPath $distDirectory).Path
    $resolvedStaging = [IO.Path]::GetFullPath($stagingRoot)
    if ((Split-Path $resolvedStaging) -ne $resolvedDist -or (Split-Path $resolvedStaging -Leaf) -ne "staging-$tagName") { throw 'Release staging path escaped the dist directory.' }
    if (Test-Path -LiteralPath $stagingRoot) {
        if ((Get-Item -LiteralPath $stagingRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Release staging cannot be a reparse point.' }
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    if (Test-Path -LiteralPath $checksumPath) {
        Remove-Item -LiteralPath $checksumPath -Force
    }

    Copy-ReleaseRuntime
    Assert-StagingContent
    $identity = Write-InnerManifest

    # Compress-Archive fails when a scanner briefly locks a freshly copied DLL.
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::Open(
        $zipPath,
        [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in @(Get-ChildItem -LiteralPath $stagingDirectory -Recurse -File)) {
            $entryName = Get-RelativeStagingPath -BaseDirectory $stagingRoot -FullName $file.FullName
            $copied = $false
            for ($attempt = 1; $attempt -le 8 -and -not $copied; $attempt++) {
                $inputStream = $null
                $outputStream = $null
                try {
                    $inputStream = [System.IO.File]::Open(
                        $file.FullName,
                        [System.IO.FileMode]::Open,
                        [System.IO.FileAccess]::Read,
                        [System.IO.FileShare]::ReadWrite)
                    $entry = $archive.CreateEntry(
                        $entryName,
                        [System.IO.Compression.CompressionLevel]::Optimal)
                    $outputStream = $entry.Open()
                    $inputStream.CopyTo($outputStream)
                    $copied = $true
                } catch [System.IO.IOException] {
                    if ($attempt -ge 8) { throw }
                    Start-Sleep -Milliseconds (250 * $attempt)
                } finally {
                    if ($null -ne $outputStream) { $outputStream.Dispose() }
                    if ($null -ne $inputStream) { $inputStream.Dispose() }
                }
            }
        }
    } finally {
        $archive.Dispose()
    }
    if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
        throw "ZIP creation failed: $zipPath"
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entryNames = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        $forbidden = @($entryNames | Where-Object {
            $_ -match '(?i)(^|/)(redclaw_host_service|redclaw_debug_bridge)\.exe$' -or
            $_ -match '(?i)\.(lib|pdb|exp|ilk|obj|log|db|sqlite|sqlite3|conf|pem|key)$' -or
            $_ -match '(?i)(^|/)(reports?|runtime-signaling|logs?|cache|credentials?)(/|$)' -or
            $_ -match '(?i)(^|/)(Qt6[^/]*d|libprotobufd|qsvgicond|qgifd|qicod|qjpegd|qsvgd|qnetworklistmanagerd|qwindowsd|qwindowsvistastyled|qcertonlybackendd|qopensslbackendd|qschannelbackendd)\.dll$' -or
            $_ -match '(?i)gd-x64-[^/]+\.dll$'
        })
        if ($forbidden.Count -gt 0) {
            throw "ZIP validation failed; forbidden entries found:`n$($forbidden -join "`n")"
        }
        if (-not ($entryNames | Where-Object { $_ -eq 'RedClawDesktop/redclaw_desktop.exe' })) {
            throw 'ZIP validation failed: RedClawDesktop/redclaw_desktop.exe is missing.'
        }
    } finally {
        $archive.Dispose()
    }

    $zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $archiveFileList = Get-StagedRelativeFiles
    $checksumLines = @(
        "# RedClawDesktop $tagName",
        "# git_sha=$($identity.GitSha)",
        "# redclaw_desktop_sha256=$($identity.ExeSha256)",
        "# archive_file_count=$($archiveFileList.Count)",
        "$zipHash  $([System.IO.Path]::GetFileName($zipPath))"
    )
    [System.IO.File]::WriteAllLines(
        $checksumPath,
        $checksumLines,
        [System.Text.UTF8Encoding]::new($false))

    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    return [pscustomobject]@{
        GitSha = $identity.GitSha
        ExeSha256 = $identity.ExeSha256
        ZipSha256 = $zipHash
        FileCount = $archiveFileList.Count
    }
}

function Publish-GitHubRelease {
    param([Parameter(Mandatory = $true)]$Identity)

    Push-Location $repoRoot
    try {
        & git tag -a $tagName -m "RedClawDesktop $tagName"
        Assert-LastExitCode -Name "git tag $tagName"
        & git push origin $tagName
        Assert-LastExitCode -Name "git push origin $tagName"

        & gh release create $tagName $zipPath $checksumPath `
            --title "RedClawDesktop $tagName Developer Preview" `
            --notes-file $releaseNotesPath `
            --prerelease `
            --verify-tag
        Assert-LastExitCode -Name "gh release create $tagName"

        $tagSha = (& git rev-list -n 1 $tagName).Trim()
        Assert-LastExitCode -Name "git rev-list $tagName"
        if ($tagSha -ne $Identity.GitSha) {
            throw "Published tag points to $tagSha instead of $($Identity.GitSha)."
        }
    } finally {
        Pop-Location
    }
}

Assert-Version
if (-not $PackageOnly) {
    Assert-PublishPreflight
}

if (-not $SkipBuild) {
    & (Join-Path $repoRoot 'build.ps1') -Configuration Release
    Assert-LastExitCode -Name 'build.ps1 -Configuration Release'
}

$identity = New-PortableArchive
$smokeReportPath = Join-Path $repoRoot "build\reports\release-$tagName-package-smoke.json"
& (Join-Path $scriptRoot 'test-portable-package.ps1') `
    -ArchivePath $zipPath -ChecksumPath $checksumPath -OutputJsonPath $smokeReportPath
Write-Host "[release] Extracted package smoke passed: $smokeReportPath" -ForegroundColor Green
Write-Host "[release] ZIP: $zipPath" -ForegroundColor Green
Write-Host "[release] ZIP SHA256: $($identity.ZipSha256)"
Write-Host "[release] EXE SHA256: $($identity.ExeSha256)"
Write-Host "[release] Git SHA: $($identity.GitSha)"
Write-Host "[release] Files: $($identity.FileCount)"

if ($PackageOnly) {
    Write-Host '[release] PackageOnly selected; tag and GitHub Release were not created.' -ForegroundColor Yellow
    exit 0
}

Publish-GitHubRelease -Identity $identity
Write-Host "[release] GitHub prerelease published: $tagName" -ForegroundColor Green
