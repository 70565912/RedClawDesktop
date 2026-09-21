param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('host', 'controller')]
    [string]$Role,

    [string]$ConnectionCredentialFile = '',

    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

function Get-PhysicalDefaultNetworkExit {
    $excludedAdapterPattern = '(?i)(virtual|vpn|tap|tun|wintun|wireguard|flclash|hyper-v|vmware|virtualbox|zerotier|tailscale|loopback)'
    $defaultRoutes = @(Get-NetRoute `
        -AddressFamily IPv4 `
        -DestinationPrefix '0.0.0.0/0' `
        -PolicyStore ActiveStore `
        -ErrorAction Stop)
    $adapters = @(Get-NetAdapter -ErrorAction Stop | Where-Object {
        $_.Status -eq 'Up' -and
        [bool]$_.HardwareInterface -and
        $_.Name -notmatch $excludedAdapterPattern -and
        $_.InterfaceDescription -notmatch $excludedAdapterPattern
    })

    $candidates = foreach ($adapter in $adapters) {
        $routes = @($defaultRoutes | Where-Object { $_.InterfaceIndex -eq $adapter.ifIndex })
        if ($routes.Count -eq 0) {
            continue
        }

        $addresses = @(Get-NetIPAddress `
            -AddressFamily IPv4 `
            -InterfaceIndex $adapter.ifIndex `
            -ErrorAction SilentlyContinue | Where-Object {
                -not $_.SkipAsSource -and
                $_.IPAddress -ne '127.0.0.1' -and
                $_.IPAddress -notlike '169.254.*'
            })
        $bestRoute = $routes | Sort-Object RouteMetric | Select-Object -First 1
        if ($addresses.Count -gt 1) {
            $sourceAddresses = @(Find-NetRoute `
                -RemoteIPAddress $bestRoute.NextHop `
                -InterfaceIndex $adapter.ifIndex `
                -ErrorAction Stop | Where-Object { $_.IPAddress } |
                Select-Object -ExpandProperty IPAddress)
            $addresses = @($addresses | Where-Object { $_.IPAddress -in $sourceAddresses })
        }
        if ($addresses.Count -ne 1) {
            continue
        }

        $ipInterface = Get-NetIPInterface `
            -AddressFamily IPv4 `
            -InterfaceIndex $adapter.ifIndex `
            -ErrorAction Stop
        [pscustomobject]@{
            Address = [string]$addresses[0].IPAddress
            Alias = [string]$adapter.Name
            Description = [string]$adapter.InterfaceDescription
            Score = [int]$bestRoute.RouteMetric + [int]$ipInterface.InterfaceMetric
        }
    }

    $ordered = @($candidates | Sort-Object Score, Alias)
    if ($ordered.Count -eq 0) {
        throw 'No active physical Ethernet/Wi-Fi IPv4 default route was found. Connect a physical network adapter and retry.'
    }
    if ($ordered.Count -gt 1 -and $ordered[0].Score -eq $ordered[1].Score) {
        $ties = ($ordered | Where-Object { $_.Score -eq $ordered[0].Score } |
            ForEach-Object { '{0} ({1})' -f $_.Alias, $_.Address }) -join ', '
        throw "Multiple physical network exits have the same route priority: $ties. Disable the unused adapter and retry."
    }
    return $ordered[0]
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$networkExit = Get-PhysicalDefaultNetworkExit
$runId = 'debug_{0}_{1}' -f $Role, (Get-Date -Format 'yyyyMMdd_HHmmss')
Write-Host ('[debug-{0}] physical network exit: {1} ({2})' -f $Role, $networkExit.Alias, $networkExit.Address)
Write-Host ('[debug-{0}] run id: {1}' -f $Role, $runId)

$arguments = @{
    Role = $Role
    Configuration = 'Debug'
    RunId = $runId
    NetworkBindAddress = $networkExit.Address
    SignalTimeoutSeconds = 0
    AutoStart = $true
}
if ($ConnectionCredentialFile) { $arguments.ConnectionCredentialFile = $ConnectionCredentialFile }
if ($DryRun) {
    $arguments.DryRun = $true
}

& (Join-Path $PSScriptRoot 'start-cross-lan-debug-supervisor.ps1') @arguments
