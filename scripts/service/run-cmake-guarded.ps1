[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingCMakeArgs,

    [string]$CMakePath = "",

    [string[]]$CMakeArgs = @(),

    [string]$CMakeArgsJson = "",

    [string]$CMakeArgsBase64 = "",

    [string]$LockName = "Global\RedClawDesktop.CMakeBuild.Lock",

    [int]$WaitTimeoutSeconds = 0,

    [ValidateRange(0, 86400)]
    [int]$CommandTimeoutSeconds = 0,

    [ValidateRange(0, 86400)]
    [int]$NoOutputTimeoutSeconds = 0,

    [string]$LogDirectory = "build\reports\cmake-guard"
)

$ErrorActionPreference = "Stop"

function Get-VsWherePath {
    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    if ([string]::IsNullOrWhiteSpace($programFilesX86)) {
        return ""
    }

    $vsWherePath = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWherePath) {
        return $vsWherePath
    }

    return ""
}

function Get-VisualStudioInstallPaths {
    $vsWherePath = Get-VsWherePath
    if ([string]::IsNullOrWhiteSpace($vsWherePath)) {
        return @()
    }

    $installPaths = & $vsWherePath -products * -property installationPath
    return @($installPaths | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object { $_.Trim() })
}

function Add-DirectoryToPathIfExists {
    param(
        [string]$Directory
    )

    if ([string]::IsNullOrWhiteSpace($Directory) -or -not (Test-Path $Directory)) {
        return
    }

    $resolvedDirectory = (Resolve-Path $Directory).Path
    $pathEntries = @($env:PATH -split ';')
    if ($pathEntries -notcontains $resolvedDirectory) {
        $env:PATH = "$resolvedDirectory;$env:PATH"
    }
}

function Resolve-CMakeExecutable {
    param(
        [string]$ExplicitPath
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        return $ExplicitPath
    }

    $pathCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $pathCommand -and -not [string]::IsNullOrWhiteSpace($pathCommand.Source)) {
        return $pathCommand.Source
    }

    $candidatePaths = @()
    $programFiles = [Environment]::GetFolderPath("ProgramFiles")
    if (-not [string]::IsNullOrWhiteSpace($programFiles)) {
        $candidatePaths += (Join-Path $programFiles "CMake\bin\cmake.exe")
    }

    foreach ($installPath in Get-VisualStudioInstallPaths) {
        $candidatePaths += (Join-Path $installPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
    }

    foreach ($candidatePath in $candidatePaths) {
        if (Test-Path $candidatePath) {
            return $candidatePath
        }
    }

    return ""
}

$CMakePath = Resolve-CMakeExecutable -ExplicitPath $CMakePath

$normalizedCMakeArgs = @()
if (-not [string]::IsNullOrWhiteSpace($CMakeArgsBase64)) {
    try {
        $decodedText = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($CMakeArgsBase64))
        if (-not [string]::IsNullOrEmpty($decodedText)) {
            $normalizedCMakeArgs += @($decodedText -split [char]31)
        }
    } catch {
        throw "Invalid -CMakeArgsBase64 value: $($_.Exception.Message)"
    }
}
if (-not [string]::IsNullOrWhiteSpace($CMakeArgsJson)) {
    try {
        $decodedCMakeArgs = $CMakeArgsJson | ConvertFrom-Json
        foreach ($decodedArg in @($decodedCMakeArgs)) {
            $normalizedCMakeArgs += [string]$decodedArg
        }
    } catch {
        throw "Invalid -CMakeArgsJson value: $($_.Exception.Message)"
    }
}
if ($null -ne $CMakeArgs) {
    $normalizedCMakeArgs += @($CMakeArgs)
}
if ($null -ne $RemainingCMakeArgs -and $RemainingCMakeArgs.Count -gt 0) {
    $normalizedCMakeArgs += @($RemainingCMakeArgs)
}
$CMakeArgs = @($normalizedCMakeArgs | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })

if ($null -eq $CMakeArgs -or $CMakeArgs.Count -eq 0) {
    throw "No CMake arguments were provided. Example: --build --preset debug-local"
}

if ($CMakeArgs.Count -eq 1 -and $CMakeArgs[0] -in @("--build", "--fresh", "--preset")) {
    throw "Incomplete CMake arguments were provided: $($CMakeArgs -join ' '). Check caller argument forwarding."
}
if ($CMakeArgs[0] -eq "--build" -and ($CMakeArgs.Count -lt 2 -or ($CMakeArgs -notcontains "--preset" -and $CMakeArgs[1].StartsWith("-")))) {
    throw "Incomplete CMake build arguments were provided: $($CMakeArgs -join ' '). Expected --build --preset <name> or --build <dir>."
}
if ($CMakeArgs[0] -eq "--fresh" -and ($CMakeArgs -notcontains "--preset")) {
    throw "Incomplete CMake configure arguments were provided: $($CMakeArgs -join ' '). Expected --fresh --preset <name>."
}

if (-not (Test-Path $CMakePath)) {
    throw "CMake executable not found. Put cmake.exe on PATH or pass -CMakePath <path>."
}

function Import-VsBundledCMakeToolPathIfNeeded {
    foreach ($installPath in Get-VisualStudioInstallPaths) {
        Add-DirectoryToPathIfExists -Directory (Join-Path $installPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")
        Add-DirectoryToPathIfExists -Directory (Join-Path $installPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin")
    }
}

function Import-VcpkgRootIfNeeded {
    if (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        $configuredVcpkg = Join-Path $env:VCPKG_ROOT "vcpkg.exe"
        if (Test-Path $configuredVcpkg) {
            return
        }

        Write-Host "[cmake-guard] VCPKG_ROOT is set but vcpkg.exe was not found: $env:VCPKG_ROOT" -ForegroundColor Yellow
    }

    foreach ($presetFile in @("CMakeUserPresets.json", "CMakePresets.json")) {
        if (-not (Test-Path $presetFile)) {
            continue
        }

        try {
            $presetDocument = Get-Content $presetFile -Raw | ConvertFrom-Json
        } catch {
            continue
        }

        foreach ($preset in @($presetDocument.configurePresets)) {
            if ($null -eq $preset.environment) {
                continue
            }

            $vcpkgRootProperty = $preset.environment.PSObject.Properties["VCPKG_ROOT"]
            if ($null -eq $vcpkgRootProperty) {
                continue
            }

            $candidateRoot = [string]$vcpkgRootProperty.Value
            if ([string]::IsNullOrWhiteSpace($candidateRoot) -or $candidateRoot.Contains("<") -or $candidateRoot.StartsWith("$")) {
                continue
            }

            $candidateVcpkg = Join-Path $candidateRoot "vcpkg.exe"
            if (Test-Path $candidateVcpkg) {
                $env:VCPKG_ROOT = $candidateRoot
                Write-Host "[cmake-guard] Resolved VCPKG_ROOT from ${presetFile}: $env:VCPKG_ROOT"
                return
            }
        }
    }

    $vcpkgCommand = Get-Command vcpkg.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $vcpkgCommand -or [string]::IsNullOrWhiteSpace($vcpkgCommand.Source)) {
        return
    }

    $env:VCPKG_ROOT = Split-Path -Parent $vcpkgCommand.Source
    Write-Host "[cmake-guard] Resolved VCPKG_ROOT from PATH: $env:VCPKG_ROOT"
}

function Write-GuardRecoveryHelp {
    Write-Host "[cmake-guard] Another build/configure operation is already in progress." -ForegroundColor Yellow
    Write-Host "[cmake-guard] Recommended recovery steps:" -ForegroundColor Yellow
    Write-Host "  1) Wait for the active build/test to finish (or stop it explicitly)." -ForegroundColor Yellow
    Write-Host "  2) Re-run configure and build in sequence:" -ForegroundColor Yellow
    Write-Host "     cmake --preset vs2022-x64-local" -ForegroundColor Yellow
    Write-Host "     cmake --build --preset debug-local" -ForegroundColor Yellow
    Write-Host "  3) If you need tests, build explicit targets:" -ForegroundColor Yellow
    Write-Host "     cmake --build --preset debug-local --target redclaw_tests_unit" -ForegroundColor Yellow
    Write-Host "[cmake-guard] Details: docs/setup/cmake-tools-api-failure-troubleshooting.md" -ForegroundColor Yellow
}

function ConvertTo-CommandLineArgument {
    param([string]$Value)

    if ($null -eq $Value) {
        return '""'
    }

    if ($Value -notmatch '[\s"]') {
        return $Value
    }

    return '"' + ($Value -replace '"', '\"') + '"'
}

function Get-CommandLogDirectory {
    param([string]$Directory)

    if ([string]::IsNullOrWhiteSpace($Directory)) {
        return ""
    }

    $resolvedDirectory = if ([System.IO.Path]::IsPathRooted($Directory)) {
        $Directory
    } else {
        Join-Path (Get-Location).Path $Directory
    }

    New-Item -ItemType Directory -Force -Path $resolvedDirectory | Out-Null
    return (Resolve-Path $resolvedDirectory).Path
}

function Read-NewFileText {
    param(
        [string]$Path,
        [ref]$Position
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path)) {
        return ""
    }

    $stream = $null
    $reader = $null
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        if ($Position.Value -gt $stream.Length) {
            $Position.Value = 0
        }
        [void]$stream.Seek([int64]$Position.Value, [System.IO.SeekOrigin]::Begin)
        $reader = New-Object System.IO.StreamReader($stream)
        $text = $reader.ReadToEnd()
        $Position.Value = $stream.Position
        return $text
    } finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        } elseif ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function Write-StreamedText {
    param(
        [string]$Text,
        [switch]$ErrorText
    )

    if ([string]::IsNullOrEmpty($Text)) {
        return
    }

    if ($ErrorText) {
        Write-Host -NoNewline $Text -ForegroundColor Red
    } else {
        Write-Host -NoNewline $Text
    }
}

function Add-ProcessTreeLine {
    param(
        [object[]]$Processes,
        [int]$ProcessId,
        [int]$Depth,
        [System.Collections.Generic.List[string]]$Lines
    )

    $processInfo = $Processes | Where-Object { [int]$_.ProcessId -eq $ProcessId } | Select-Object -First 1
    if ($null -eq $processInfo) {
        $Lines.Add(("[cmake-guard] {0}pid={1} <already exited>" -f ("  " * $Depth), $ProcessId)) | Out-Null
        return
    }

    $commandLine = [string]$processInfo.CommandLine
    if ($commandLine.Length -gt 220) {
        $commandLine = $commandLine.Substring(0, 217) + "..."
    }

    $Lines.Add(("[cmake-guard] {0}pid={1} ppid={2} name={3} started={4} cmd={5}" -f `
        ("  " * $Depth), `
        $processInfo.ProcessId, `
        $processInfo.ParentProcessId, `
        $processInfo.Name, `
        $processInfo.CreationDate, `
        $commandLine)) | Out-Null

    foreach ($child in @($Processes | Where-Object { [int]$_.ParentProcessId -eq $ProcessId } | Sort-Object ProcessId)) {
        Add-ProcessTreeLine -Processes $Processes -ProcessId ([int]$child.ProcessId) -Depth ($Depth + 1) -Lines $Lines
    }
}

function Write-ProcessSnapshot {
    param([int]$RootProcessId)

    $lines = [System.Collections.Generic.List[string]]::new()
    try {
        $processes = @(Get-CimInstance Win32_Process)
        $lines.Add("[cmake-guard] process tree snapshot before timeout cleanup:") | Out-Null
        Add-ProcessTreeLine -Processes $processes -ProcessId $RootProcessId -Depth 0 -Lines $lines

        $interestingNames = @(
            "cmake.exe",
            "ninja.exe",
            "cl.exe",
            "c1xx.exe",
            "c2.exe",
            "link.exe",
            "lib.exe",
            "mspdbsrv.exe",
            "cmd.exe",
            "conhost.exe",
            "rc.exe",
            "mt.exe",
            "pwsh.exe",
            "powershell.exe",
            "vcpkg.exe"
        )
        $relatedProcesses = @(
            $processes |
                Where-Object { $interestingNames -contains $_.Name } |
                Sort-Object Name, ProcessId
        )
        if ($relatedProcesses.Count -gt 0) {
            $lines.Add("[cmake-guard] related build-tool process snapshot:") | Out-Null
            foreach ($processInfo in $relatedProcesses) {
                $commandLine = [string]$processInfo.CommandLine
                if ($commandLine.Length -gt 220) {
                    $commandLine = $commandLine.Substring(0, 217) + "..."
                }
                $lines.Add(("[cmake-guard] pid={0} ppid={1} name={2} started={3} cmd={4}" -f `
                    $processInfo.ProcessId, `
                    $processInfo.ParentProcessId, `
                    $processInfo.Name, `
                    $processInfo.CreationDate, `
                    $commandLine)) | Out-Null
            }
        }
    } catch {
        $lines.Add("[cmake-guard] process snapshot failed: $($_.Exception.Message)") | Out-Null
        $fallbackProcesses = @(
            Get-Process `
                cmake, ninja, cl, c1xx, c2, link, lib, mspdbsrv, cmd, conhost, rc, mt, pwsh, powershell, vcpkg `
                -ErrorAction SilentlyContinue |
                Sort-Object ProcessName, Id
        )
        if ($fallbackProcesses.Count -gt 0) {
            $lines.Add("[cmake-guard] related build-tool process fallback snapshot:") | Out-Null
            foreach ($processInfo in $fallbackProcesses) {
                $lines.Add(("[cmake-guard] pid={0} name={1} started={2} path={3}" -f `
                    $processInfo.Id, `
                    $processInfo.ProcessName, `
                    $processInfo.StartTime, `
                    $processInfo.Path)) | Out-Null
            }
        }
    }

    foreach ($line in $lines) {
        Write-Host $line -ForegroundColor Yellow
    }
}

function Stop-ProcessTree {
    param([int]$RootProcessId)

    $taskkillPath = Join-Path $env:WINDIR "System32\taskkill.exe"
    if (Test-Path $taskkillPath) {
        try {
            $taskkillOutput = & $taskkillPath /PID $RootProcessId /T /F 2>&1
            foreach ($line in @($taskkillOutput)) {
                if (-not [string]::IsNullOrWhiteSpace($line)) {
                    Write-Host "[cmake-guard] taskkill: $line" -ForegroundColor Yellow
                }
            }
            return
        } catch {
            Write-Host "[cmake-guard] taskkill failed for PID ${RootProcessId}: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }

    try {
        $children = @(Get-CimInstance Win32_Process | Where-Object { $_.ParentProcessId -eq $RootProcessId })
        foreach ($child in $children) {
            Stop-ProcessTree -RootProcessId ([int]$child.ProcessId)
        }
    } catch {
        Write-Host "[cmake-guard] child-process enumeration failed for PID ${RootProcessId}: $($_.Exception.Message)" -ForegroundColor Yellow
    } finally {
        $process = Get-Process -Id $RootProcessId -ErrorAction SilentlyContinue
        if ($null -ne $process) {
            Stop-Process -Id $RootProcessId -Force
        }
    }
}

function Invoke-CMakeWithWatchdog {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [int]$TimeoutSeconds,
        [int]$QuietTimeoutSeconds,
        [string]$Directory,
        [Parameter(Mandatory = $true)]
        [ref]$ExitCode
    )

    $ExitCode.Value = 0

    $runId = "{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss_fff"), ([guid]::NewGuid().ToString("N").Substring(0, 6))
    $resolvedLogDirectory = Get-CommandLogDirectory -Directory $Directory
    $stdoutPath = if ([string]::IsNullOrWhiteSpace($resolvedLogDirectory)) { "" } else { Join-Path $resolvedLogDirectory ("cmake-{0}.out.log" -f $runId) }
    $stderrPath = if ([string]::IsNullOrWhiteSpace($resolvedLogDirectory)) { "" } else { Join-Path $resolvedLogDirectory ("cmake-{0}.err.log" -f $runId) }
    $exitCodeDirectory = if ([string]::IsNullOrWhiteSpace($resolvedLogDirectory)) {
        [System.IO.Path]::GetTempPath()
    } else {
        $resolvedLogDirectory
    }
    $exitCodePath = Join-Path $exitCodeDirectory ("cmake-{0}.exit" -f $runId)
    $childScript = Join-Path $PSScriptRoot 'invoke-cmake-guard-child.ps1'
    if (-not (Test-Path -LiteralPath $childScript -PathType Leaf)) {
        throw "CMake guard child script not found: $childScript"
    }
    $executableBase64 = [Convert]::ToBase64String(
        [Text.Encoding]::UTF8.GetBytes($Executable))
    $argumentsText = [string]::Join([char]31, @($Arguments))
    $argumentsBase64 = [Convert]::ToBase64String(
        [Text.Encoding]::UTF8.GetBytes($argumentsText))
    $exitCodePathBase64 = [Convert]::ToBase64String(
        [Text.Encoding]::UTF8.GetBytes($exitCodePath))
    $childArguments = @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        $childScript,
        '-ExecutableBase64',
        $executableBase64,
        '-ArgumentsBase64',
        $argumentsBase64,
        '-ExitCodePathBase64',
        $exitCodePathBase64)
    $argumentLine = ($childArguments |
        ForEach-Object { ConvertTo-CommandLineArgument -Value $_ }) -join " "

    Write-Host "[cmake-guard] watchdog enabled: command_timeout=${TimeoutSeconds}s no_output_timeout=${QuietTimeoutSeconds}s"
    if (-not [string]::IsNullOrWhiteSpace($stdoutPath)) {
        Write-Host "[cmake-guard] stdout log: $stdoutPath"
        Write-Host "[cmake-guard] stderr log: $stderrPath"
    }

    $startInfo = @{
        FilePath = (Join-Path $PSHOME 'powershell.exe')
        ArgumentList = $argumentLine
        PassThru = $true
        WindowStyle = "Hidden"
    }
    if (-not [string]::IsNullOrWhiteSpace($stdoutPath)) {
        $startInfo.RedirectStandardOutput = $stdoutPath
        $startInfo.RedirectStandardError = $stderrPath
    }

    $process = Start-Process @startInfo
    $startedAt = Get-Date
    $lastOutputAt = $startedAt
    $lastHeartbeatAt = $startedAt
    [int64]$stdoutPosition = 0
    [int64]$stderrPosition = 0

    while (-not $process.HasExited) {
        Start-Sleep -Seconds 2

        $newStdout = Read-NewFileText -Path $stdoutPath -Position ([ref]$stdoutPosition)
        $newStderr = Read-NewFileText -Path $stderrPath -Position ([ref]$stderrPosition)
        if (-not [string]::IsNullOrEmpty($newStdout)) {
            Write-StreamedText -Text $newStdout
            $lastOutputAt = Get-Date
        }
        if (-not [string]::IsNullOrEmpty($newStderr)) {
            Write-StreamedText -Text $newStderr -ErrorText
            $lastOutputAt = Get-Date
        }

        $now = Get-Date
        $elapsedSeconds = [int](($now - $startedAt).TotalSeconds)
        $quietSeconds = [int](($now - $lastOutputAt).TotalSeconds)

        if ($TimeoutSeconds -gt 0 -and $elapsedSeconds -ge $TimeoutSeconds) {
            Write-Host "[cmake-guard] CMake command timed out after ${elapsedSeconds}s; stopping process tree rooted at PID $($process.Id)." -ForegroundColor Red
            Write-ProcessSnapshot -RootProcessId $process.Id
            Stop-ProcessTree -RootProcessId $process.Id
            $ExitCode.Value = 124
            return
        }

        if ($QuietTimeoutSeconds -gt 0 -and $quietSeconds -ge $QuietTimeoutSeconds) {
            Write-Host "[cmake-guard] CMake command produced no output for ${quietSeconds}s; stopping process tree rooted at PID $($process.Id)." -ForegroundColor Red
            Write-ProcessSnapshot -RootProcessId $process.Id
            Stop-ProcessTree -RootProcessId $process.Id
            $ExitCode.Value = 125
            return
        }

        if (($now - $lastHeartbeatAt).TotalSeconds -ge 30) {
            Write-Host "[cmake-guard] still running pid=$($process.Id) elapsed=${elapsedSeconds}s quiet=${quietSeconds}s"
            $lastHeartbeatAt = $now
        }

        $process.Refresh()
    }

    $process.WaitForExit()
    $process.Refresh()

    $newStdout = Read-NewFileText -Path $stdoutPath -Position ([ref]$stdoutPosition)
    $newStderr = Read-NewFileText -Path $stderrPath -Position ([ref]$stderrPosition)
    Write-StreamedText -Text $newStdout
    Write-StreamedText -Text $newStderr -ErrorText

    $recordedExitCode = 126
    if (Test-Path -LiteralPath $exitCodePath -PathType Leaf) {
        $exitCodeText = (Get-Content -LiteralPath $exitCodePath -Raw).Trim()
        $parsedExitCode = 0
        if ([int]::TryParse($exitCodeText, [ref]$parsedExitCode)) {
            $recordedExitCode = $parsedExitCode
        }
        Remove-Item -LiteralPath $exitCodePath -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host "[cmake-guard] Child exit-code record is missing; treating the command as failed." -ForegroundColor Red
    }
    $ExitCode.Value = $recordedExitCode
}

function Import-VsDeveloperEnvironmentIfNeeded {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return
    }

    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    $vsWherePath = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWherePath)) {
        throw "MSVC compiler not found in PATH and vswhere.exe is unavailable at '$vsWherePath'."
    }

    $installPath = & $vsWherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $installPath = ($installPath | Select-Object -First 1).Trim()
    if ([string]::IsNullOrWhiteSpace($installPath)) {
        throw "MSVC compiler not found in PATH and no Visual Studio installation with C++ tools was discovered via vswhere."
    }

    $vsDevCmdPath = Join-Path $installPath "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmdPath)) {
        throw "Could not find VsDevCmd.bat at '$vsDevCmdPath'."
    }

    $envDump = & cmd.exe /d /s /c "`"$vsDevCmdPath`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize Visual Studio developer environment via '$vsDevCmdPath'."
    }

    foreach ($entry in $envDump) {
        if ($entry -match '^(?<Name>[^=]+)=(?<Value>.*)$') {
            Set-Item -Path ("Env:" + $matches.Name) -Value $matches.Value
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "Visual Studio developer environment was loaded, but cl.exe is still not available in PATH."
    }

    Write-Host "[cmake-guard] Imported Visual Studio developer environment for MSVC toolchain."
}

$mutex = New-Object System.Threading.Mutex($false, $LockName)
$acquired = $false

try {
    $acquired = $mutex.WaitOne([TimeSpan]::FromSeconds($WaitTimeoutSeconds))
    if (-not $acquired) {
        Write-Host "[cmake-guard] Another build/configure operation is already in progress (lock: $LockName)." -ForegroundColor Red
        Write-GuardRecoveryHelp
        exit 42
    }

    Write-Host "[cmake-guard] lock acquired: $LockName"
    Write-Host "[cmake-guard] command: $CMakePath $($CMakeArgs -join ' ')"

    Import-VsBundledCMakeToolPathIfNeeded
    Import-VcpkgRootIfNeeded
    Import-VsDeveloperEnvironmentIfNeeded

    [int]$exitCode = 0
    if ($CommandTimeoutSeconds -gt 0 -or $NoOutputTimeoutSeconds -gt 0) {
        [int]$watchdogExitCode = 0
        Invoke-CMakeWithWatchdog `
            -Executable $CMakePath `
            -Arguments $CMakeArgs `
            -TimeoutSeconds $CommandTimeoutSeconds `
            -QuietTimeoutSeconds $NoOutputTimeoutSeconds `
            -Directory $LogDirectory `
            -ExitCode ([ref]$watchdogExitCode)
        $exitCode = $watchdogExitCode
    } else {
        & $CMakePath @CMakeArgs
        $exitCode = $LASTEXITCODE
    }

    if ($exitCode -ne 0) {
        Write-Host "[cmake-guard] CMake command failed with exit code $exitCode" -ForegroundColor Red
    }

    exit $exitCode
}
finally {
    if ($acquired) {
        $mutex.ReleaseMutex()
    }

    if ($null -ne $mutex) {
        $mutex.Dispose()
    }
}
