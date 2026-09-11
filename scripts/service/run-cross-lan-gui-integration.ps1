param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('host', 'controller')]
    [string]$Role,

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$RuntimeExe = '',

    [string]$SessionCode = '',

    [string]$RunId = '',

    [string]$ReportRoot = 'build\reports',

    [string[]]$IceServer = @(),

    [string]$IceServerFile = '',

    [string]$ControlName = 'RedClawDesktop.DebugControl.v1',

    [string]$AgentControlName = 'RedClawDesktop.AgentControl.v1',

    [string]$CoordinationJournalPath = '',

    [switch]$AllowRemoteAgent,

    [string[]]$AgentProjectRoot = @(),

    [ValidateRange(-1, 3600)]
    [int]$SignalTimeoutSeconds = -1,

    [ValidateRange(0, 65535)]
    [int]$DhtListenPort = 0,

    [ValidateRange(1, 65535)]
    [int]$IceUdpPort = 55000,

    [string]$NetworkBindAddress = '',

    [switch]$SkipPrep,

    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'cross-lan-integration-config.ps1')

if ($SignalTimeoutSeconds -lt 0) {
    $SignalTimeoutSeconds = $script:CrossLanIntegrationSignalTimeoutSeconds
}

$supervisorArguments = @{
    Role = $Role
    Configuration = $Configuration
    SignalTimeoutSeconds = $SignalTimeoutSeconds
    DhtListenPort = $DhtListenPort
    IceUdpPort = $IceUdpPort
    AutoStart = $true
    AgentControlName = $AgentControlName
    CoordinationJournalPath = $CoordinationJournalPath
    AllowRemoteAgent = [bool]$AllowRemoteAgent
    AgentProjectRoot = $AgentProjectRoot
}

if (-not [string]::IsNullOrWhiteSpace($RuntimeExe)) {
    $supervisorArguments.RuntimeExe = $RuntimeExe
}
if (-not [string]::IsNullOrWhiteSpace($SessionCode)) {
    $supervisorArguments.SessionCode = $SessionCode
}
if (-not [string]::IsNullOrWhiteSpace($RunId)) {
    $supervisorArguments.RunId = $RunId
}
if (-not [string]::IsNullOrWhiteSpace($ReportRoot)) {
    $supervisorArguments.ReportRoot = $ReportRoot
}
if ($IceServer.Count -gt 0) {
    $supervisorArguments.IceServer = $IceServer
}
if (-not [string]::IsNullOrWhiteSpace($IceServerFile)) {
    $supervisorArguments.IceServerFile = $IceServerFile
}
if (-not [string]::IsNullOrWhiteSpace($ControlName)) {
    $supervisorArguments.ControlName = $ControlName
}
if (-not [string]::IsNullOrWhiteSpace($NetworkBindAddress)) {
    $supervisorArguments.NetworkBindAddress = $NetworkBindAddress
}
if ($SkipPrep) {
    $supervisorArguments.SkipPrep = $true
}
if ($DryRun) {
    $supervisorArguments.DryRun = $true
}

Write-Host '[cross-lan-gui] launching through the controlled GUI supervisor'
& (Join-Path $PSScriptRoot 'start-cross-lan-debug-supervisor.ps1') @supervisorArguments
