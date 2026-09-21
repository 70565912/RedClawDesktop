param(
    [string]$ConnectionCredentialFile = '',

    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$arguments = @{
    Role = 'controller'
}
if ($ConnectionCredentialFile) { $arguments.ConnectionCredentialFile = $ConnectionCredentialFile }
if ($DryRun) {
    $arguments.DryRun = $true
}

& (Join-Path $PSScriptRoot 'start-debug-role.ps1') @arguments
