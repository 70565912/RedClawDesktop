param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutableBase64,

    [Parameter(Mandatory = $true)]
    [string]$ArgumentsBase64,

    [Parameter(Mandatory = $true)]
    [string]$ExitCodePathBase64
)

$ErrorActionPreference = 'Stop'
$childExitCode = 1

try {
    $executable = [Text.Encoding]::UTF8.GetString(
        [Convert]::FromBase64String($ExecutableBase64))
    $argumentsText = [Text.Encoding]::UTF8.GetString(
        [Convert]::FromBase64String($ArgumentsBase64))
    $argumentList = if ([string]::IsNullOrEmpty($argumentsText)) {
        @()
    } else {
        @($argumentsText -split [char]31)
    }
    $exitCodePath = [Text.Encoding]::UTF8.GetString(
        [Convert]::FromBase64String($ExitCodePathBase64))

    & $executable @argumentList
    $childExitCode = if ($null -eq $LASTEXITCODE) { 0 } else { [int]$LASTEXITCODE }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    $childExitCode = 1
} finally {
    if (-not [string]::IsNullOrWhiteSpace($exitCodePath)) {
        [IO.File]::WriteAllText(
            $exitCodePath,
            [string]$childExitCode,
            [Text.UTF8Encoding]::new($false))
    }
}

exit $childExitCode
