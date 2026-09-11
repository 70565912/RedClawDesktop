param()

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$guardScript = Join-Path $PSScriptRoot 'run-cmake-guarded.ps1'
$cmdPath = Join-Path $env:WINDIR 'System32\cmd.exe'
$reportDirectory = Join-Path $repoRoot 'build\reports\cmake-guard-exit-code-selftest'

if (-not (Test-Path -LiteralPath $guardScript -PathType Leaf)) {
    throw "CMake guard script not found: $guardScript"
}
if (-not (Test-Path -LiteralPath $cmdPath -PathType Leaf)) {
    throw "cmd.exe not found: $cmdPath"
}

function ConvertTo-ArgsBase64 {
    param([string[]]$ArgumentList)

    $joined = [string]::Join([char]31, @($ArgumentList))
    return [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($joined))
}

function Invoke-GuardProbe {
    param([int]$ExpectedExitCode)

    $encodedArguments = ConvertTo-ArgsBase64 -ArgumentList @(
        '/d',
        '/c',
        "exit $ExpectedExitCode")
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $guardScript `
        -CMakePath $cmdPath `
        -CMakeArgsBase64 $encodedArguments `
        -CommandTimeoutSeconds 30 `
        -NoOutputTimeoutSeconds 20 `
        -LogDirectory $reportDirectory 2>&1
    $actualExitCode = $LASTEXITCODE
    return [pscustomobject]@{
        Expected = $ExpectedExitCode
        Actual = $actualExitCode
        Output = @($output) -join "`n"
    }
}

$failure = Invoke-GuardProbe -ExpectedExitCode 17
if ($failure.Actual -ne 17) {
    throw "Guard lost failing child exit code: expected 17, actual $($failure.Actual)"
}
if ($failure.Output -notmatch 'CMake command failed with exit code 17') {
    throw 'Guard did not report the failing child exit code.'
}

$success = Invoke-GuardProbe -ExpectedExitCode 0
if ($success.Actual -ne 0) {
    throw "Guard changed successful child exit code: expected 0, actual $($success.Actual)"
}

[pscustomobject]@{
    schema = 'redclaw.cmake-guard-exit-code-selftest.v1'
    passed = $true
    failure_exit_code = $failure.Actual
    success_exit_code = $success.Actual
} | ConvertTo-Json
