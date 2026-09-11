$ErrorActionPreference = 'Stop'

# Load the production argument builder, without running any process or DHT.
$tokens = $null
$errors = $null
$source = Join-Path $PSScriptRoot 'run-local-dual-gui-integration-test.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Local dual GUI script has syntax errors.' }
$definition = $ast.Find({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Get-IntegrationArgumentList'
}, $false)
if ($null -eq $definition) { throw 'Production argument builder missing.' }
. ([scriptblock]::Create($definition.Extent.Text))

$SessionCode = 'TEST0001'
$SignalTransport = 'dht'
$NetworkBindAddress = ''
$Configuration = 'Debug'
$AllowRemoteInput = $false
$DropOneMediaFragment = $false
$ForceRequiredChannelClose = 'none'
$ForceAgentChannelCloseAfterEvent = $false
$AgentFixtureProvider = $true
$AgentProjectRoot = $PSScriptRoot
$ControllerAgentProjectRoot = $PSScriptRoot
$script:CrossLanIntegrationIceServers = @()
$script:HostDhtListenPort = 51001
$script:ControllerDhtListenPort = 51002
$script:HostIceUdpPort = 55000
$script:ControllerIceUdpPort = 55001

function Get-ArgumentValue {
    param([object[]]$Values, [string]$Name)
    $index = [array]::IndexOf($Values, $Name)
    if ($index -lt 0 -or $index + 1 -ge $Values.Count) { throw "Missing argument: $Name" }
    return [string]$Values[$index + 1]
}

$launches = @{}
foreach ($roleName in @('host', 'controller')) {
    $testArguments = @{
        Role = $roleName
        RoleDirectory = $roleName
        SignalDirectory = 'fixture-signaling'
        ControlName = "fixture.$roleName"
        CurrentRunId = 'fixture'
    }
    $values = @(Get-IntegrationArgumentList @testArguments)
    foreach ($flag in @('--enable-agent-control', '--allow-remote-agent', '--agent-qa-fixture-provider')) {
        if ($flag -notin $values) { throw "Missing $flag for $roleName" }
    }
    if ((Get-ArgumentValue $values '--gui-role') -ne $roleName) { throw 'Desktop role changed.' }
    $launches[$roleName] = $values
}
foreach ($name in @('--agent-control-name', '--coordination-journal-path', '--debug-control-name')) {
    if ((Get-ArgumentValue $launches.host $name) -eq (Get-ArgumentValue $launches.controller $name)) {
        throw "Dual GUI isolation failed: $name"
    }
}
if ('--stream-require-capture' -notin $launches.host -or
    '--stream-require-capture' -in $launches.controller) { throw 'Capture role was changed.' }
if ((Get-ArgumentValue $launches.host '--ice-udp-port') -ne '55000') {
    throw 'Host ICE UDP port must remain fixed at 55000.'
}
if ((Get-ArgumentValue $launches.controller '--ice-udp-port') -ne '55001') {
    throw 'Controller ICE UDP port must remain fixed at 55001 for same-machine testing.'
}

$Configuration = 'Release'
$rejected = $false
try { $null = Get-IntegrationArgumentList @testArguments } catch {
    $rejected = $_.Exception.Message -eq 'AgentFixtureProvider is Debug-only.'
}
if (-not $rejected) { throw 'Release must reject the Fake provider switch.' }
# Also compile the embedded capture helper; a valid PowerShell here-string can
# otherwise hide malformed C# until the real GUI evidence step.
$captureDefinition = $ast.Find({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Initialize-WindowCaptureType'
}, $false)
if ($null -eq $captureDefinition) { throw 'Capture helper missing.' }
. ([scriptblock]::Create($captureDefinition.Extent.Text))
Initialize-WindowCaptureType
Write-Output 'Passed: both Agent API roles, isolated API/journal/Debug pipes, fixed dual ICE ports, unchanged capture role, Release fixture rejection.'
