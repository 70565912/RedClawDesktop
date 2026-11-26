param(
    [string]$ConnectionCredentialFile = '',

    [string]$SessionCode = '',

    [switch]$NoDesktopVideo,

    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$peerCodePath = Join-Path $PSScriptRoot '..\..\local\controller-session-code.txt'
if ([string]::IsNullOrWhiteSpace($SessionCode) -and (Test-Path -LiteralPath $peerCodePath -PathType Leaf)) {
    . (Join-Path $PSScriptRoot 'cross-lan-integration-config.ps1')
    $SessionCode = Resolve-CrossLanIntegrationSessionCode -LocalConfigPath $peerCodePath
}

$arguments = @{
    Role = 'controller'
    NoDesktopVideo = [bool]$NoDesktopVideo
}
if ($ConnectionCredentialFile) { $arguments.ConnectionCredentialFile = $ConnectionCredentialFile }
if ($SessionCode) { $arguments.SessionCode = $SessionCode }
if ($DryRun) {
    $arguments.DryRun = $true
}

& (Join-Path $PSScriptRoot 'start-debug-role.ps1') @arguments
