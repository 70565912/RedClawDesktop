param(
    [string]$ConnectionCredentialFile = '',

    [string]$SessionCode = '',

    [ValidateRange(1, 65535)]
    [int]$IceUdpPort,

    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$localPort = Join-Path $PSScriptRoot '..\..\local\debug-host-ice-port.txt'
if (-not $PSBoundParameters.ContainsKey('IceUdpPort') -and (Test-Path -LiteralPath $localPort -PathType Leaf)) {
    $portText = (Get-Content -LiteralPath $localPort -Raw).Trim()
    $parsedPort = 0
    if (-not [int]::TryParse($portText, [ref]$parsedPort) -or $parsedPort -lt 1 -or $parsedPort -gt 65535) {
        throw 'Local Debug Host ICE UDP port must be an integer from 1 through 65535.'
    }
    $IceUdpPort = $parsedPort
}

$arguments = @{
    Role = 'host'
}
if ($ConnectionCredentialFile) { $arguments.ConnectionCredentialFile = $ConnectionCredentialFile }
if ($SessionCode) { $arguments.SessionCode = $SessionCode }
if ($IceUdpPort) { $arguments.IceUdpPort = $IceUdpPort }
if ($DryRun) {
    $arguments.DryRun = $true
}

& (Join-Path $PSScriptRoot 'start-debug-role.ps1') @arguments
