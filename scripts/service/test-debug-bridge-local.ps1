param(
    [string]$Executable = '',

    [ValidateRange(10, 180)]
    [int]$TimeoutSeconds = 45,

    [switch]$RequireRedClawRelay,

    [switch]$KeepProcesses
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $repoRoot 'build\vs2022-x64\src\Debug\redclaw_debug_bridge.exe'
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Debug bridge executable not found: $Executable"
}

$runId = [DateTimeOffset]::Now.ToString('yyyyMMdd_HHmmss_fff')
$reportRoot = Join-Path $repoRoot "build\reports\debug-bridge-local-$runId"
$passphrasePath = Join-Path $reportRoot 'passphrase.txt'
New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null

$secretBytes = [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
[IO.File]::WriteAllText(
    $passphrasePath,
    [Convert]::ToHexString($secretBytes),
    [Text.UTF8Encoding]::new($false))
$currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
$fileSecurity = [Security.AccessControl.FileSecurity]::new()
$fileSecurity.SetOwner($currentSid)
$fileSecurity.SetAccessRuleProtection($true, $false)
$fileSecurity.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
    $currentSid,
    [Security.AccessControl.FileSystemRights]::FullControl,
    [Security.AccessControl.AccessControlType]::Allow))
Set-Acl -LiteralPath $passphrasePath -AclObject $fileSecurity

$processIds = [Collections.Generic.List[int]]::new()

function Start-BridgePair {
    param(
        [Parameter(Mandatory = $true)][string]$Epoch,
        [Parameter(Mandatory = $true)][string]$Suffix,
        [Parameter(Mandatory = $true)][string]$SignalDirectory,
        [Parameter(Mandatory = $true)][string]$HostCache,
        [Parameter(Mandatory = $true)][string]$ControllerCache
    )

    $hostControl = "RedClawDesktop.DebugBridge.LocalHost.$Suffix"
    $controllerControl = "RedClawDesktop.DebugBridge.LocalController.$Suffix"
    $hostLaunch = (& (Join-Path $repoRoot 'scripts\service\start-debug-bridge.ps1') `
        -Role host -SignalDirectory $SignalDirectory -BridgeId local-selftest `
        -SessionEpoch $Epoch -PassphraseFile $passphrasePath `
        -NetworkFingerprint local-selftest-network -RouteCache $HostCache `
        -ControlName $hostControl -RunSeconds ($TimeoutSeconds + 30) `
        -Executable $Executable -ReportDirectory (Join-Path $reportRoot "host-$Suffix") |
        Out-String | ConvertFrom-Json)
    $controllerLaunch = (& (Join-Path $repoRoot 'scripts\service\start-debug-bridge.ps1') `
        -Role controller -SignalDirectory $SignalDirectory -BridgeId local-selftest `
        -SessionEpoch $Epoch -PassphraseFile $passphrasePath `
        -NetworkFingerprint local-selftest-network -RouteCache $ControllerCache `
        -ControlName $controllerControl -RunSeconds ($TimeoutSeconds + 30) `
        -Executable $Executable -ReportDirectory (Join-Path $reportRoot "controller-$Suffix") |
        Out-String | ConvertFrom-Json)
    $processIds.Add([int]$hostLaunch.pid)
    $processIds.Add([int]$controllerLaunch.pid)
    return [pscustomobject]@{
        HostControl = $hostControl
        ControllerControl = $controllerControl
        HostPid = [int]$hostLaunch.pid
        ControllerPid = [int]$controllerLaunch.pid
    }
}

function Get-BridgeStatus {
    param([Parameter(Mandatory = $true)][string]$ControlName)
    try {
        return (& (Join-Path $repoRoot 'scripts\service\invoke-debug-bridge.ps1') `
            -Action bridge_status -ControlName $ControlName -TimeoutMs 3000 -Json |
            Out-String | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Wait-BridgePair {
    param([Parameter(Mandatory = $true)]$Pair)
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    $hostStatus = $null
    $controllerStatus = $null
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        $hostStatus = Get-BridgeStatus -ControlName $Pair.HostControl
        $controllerStatus = Get-BridgeStatus -ControlName $Pair.ControllerControl
        if ($null -ne $hostStatus -and $null -ne $controllerStatus -and
            [bool]$hostStatus.ice_connected -and [bool]$hostStatus.channel_open -and
            [bool]$controllerStatus.ice_connected -and [bool]$controllerStatus.channel_open) {
            return [pscustomobject]@{ Host = $hostStatus; Controller = $controllerStatus }
        }
        Start-Sleep -Milliseconds 500
    }
    return [pscustomobject]@{ Host = $hostStatus; Controller = $controllerStatus }
}

function Stop-VerifiedBridgePair {
    param([Parameter(Mandatory = $true)]$Pair)
    foreach ($processId in @($Pair.HostPid, $Pair.ControllerPid)) {
        $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($null -ne $process -and $process.ProcessName -eq 'redclaw_debug_bridge') {
            Stop-Process -Id $processId -Force
        }
    }
}

$result = [ordered]@{
    schema = 'redclaw.debug-bridge-local-test.v1'
    run_id = $runId
    passed = $false
    first_pair_connected = $false
    first_host_description_ready = $false
    first_controller_description_ready = $false
    first_host_description_is_offer = $false
    first_controller_description_is_offer = $false
    first_host_description_role_matches = $false
    first_controller_description_role_matches = $false
    first_host_has_ice_ufrag = $false
    first_controller_has_ice_ufrag = $false
    first_host_has_ice_pwd = $false
    first_controller_has_ice_pwd = $false
    first_host_has_fingerprint = $false
    first_controller_has_fingerprint = $false
    first_host_candidate_count = 0
    first_controller_candidate_count = 0
    first_host_signal_publish_total = 0
    first_controller_signal_publish_total = 0
    first_host_signal_publish_state = ''
    first_controller_signal_publish_state = ''
    first_host_signal_publish_error = ''
    first_controller_signal_publish_error = ''
    remote_bridge_roundtrip = $false
    redclaw_relay_ok = $false
    remote_redaction_ok = $false
    cache_pair_connected = $false
    host_cache_hit = $false
    controller_cache_hit = $false
    executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    report_root = $reportRoot
    error = ''
}

try {
    $hostCache = Join-Path $reportRoot 'host-route-cache.bin'
    $controllerCache = Join-Path $reportRoot 'controller-route-cache.bin'
    $firstSignal = Join-Path $reportRoot 'signal-first'
    $firstPair = Start-BridgePair -Epoch 'epoch-local-selftest-1' -Suffix 'first' `
        -SignalDirectory $firstSignal -HostCache $hostCache -ControllerCache $controllerCache
    $firstStatus = Wait-BridgePair -Pair $firstPair
    if ($null -ne $firstStatus.Host) {
        $result.first_host_description_ready = [bool]$firstStatus.Host.local_description_ready
        $result.first_host_description_is_offer = [bool]$firstStatus.Host.local_description_is_offer
        $result.first_host_description_role_matches = [bool]$firstStatus.Host.local_description_role_matches
        $result.first_host_has_ice_ufrag = [bool]$firstStatus.Host.local_has_ice_ufrag
        $result.first_host_has_ice_pwd = [bool]$firstStatus.Host.local_has_ice_pwd
        $result.first_host_has_fingerprint = [bool]$firstStatus.Host.local_has_fingerprint
        $result.first_host_candidate_count = [int]$firstStatus.Host.local_candidate_count
        $result.first_host_signal_publish_total = [int64]$firstStatus.Host.local_signal_publish_total
        $result.first_host_signal_publish_state = [string]$firstStatus.Host.local_signal_publish_state
        $result.first_host_signal_publish_error = [string]$firstStatus.Host.last_signal_publish_error
    }
    if ($null -ne $firstStatus.Controller) {
        $result.first_controller_description_ready = [bool]$firstStatus.Controller.local_description_ready
        $result.first_controller_description_is_offer = [bool]$firstStatus.Controller.local_description_is_offer
        $result.first_controller_description_role_matches = [bool]$firstStatus.Controller.local_description_role_matches
        $result.first_controller_has_ice_ufrag = [bool]$firstStatus.Controller.local_has_ice_ufrag
        $result.first_controller_has_ice_pwd = [bool]$firstStatus.Controller.local_has_ice_pwd
        $result.first_controller_has_fingerprint = [bool]$firstStatus.Controller.local_has_fingerprint
        $result.first_controller_candidate_count = [int]$firstStatus.Controller.local_candidate_count
        $result.first_controller_signal_publish_total = [int64]$firstStatus.Controller.local_signal_publish_total
        $result.first_controller_signal_publish_state = [string]$firstStatus.Controller.local_signal_publish_state
        $result.first_controller_signal_publish_error = [string]$firstStatus.Controller.last_signal_publish_error
    }
    $result.first_pair_connected =
        [bool]$firstStatus.Host.channel_open -and [bool]$firstStatus.Controller.channel_open
    if (-not $result.first_pair_connected) {
        throw 'First local bridge pair did not connect before timeout.'
    }

    $remoteStatus = (& (Join-Path $repoRoot 'scripts\service\invoke-debug-bridge.ps1') `
        -Action remote_bridge_status -ControlName $firstPair.ControllerControl `
        -TimeoutMs 10000 -Json | Out-String | ConvertFrom-Json)
    $result.remote_bridge_roundtrip = [bool]$remoteStatus.ok -and $remoteStatus.role -eq 'host'

    try {
        $redclawStatus = (& (Join-Path $repoRoot 'scripts\service\invoke-debug-bridge.ps1') `
            -Action status -ControlName $firstPair.ControllerControl `
            -TimeoutMs 15000 -Json | Out-String | ConvertFrom-Json)
        $result.redclaw_relay_ok = [bool]$redclawStatus.ok
        $redclawStatusText = $redclawStatus | ConvertTo-Json -Depth 20 -Compress
        $result.remote_redaction_ok =
            [string]$redclawStatus.status.network_bind_address -eq '[redacted-network-address]' -and
            $redclawStatusText -notmatch '[A-Za-z]:[\\/]'
    } catch {
        $result.redclaw_relay_ok = $false
        if ($RequireRedClawRelay) {
            throw
        }
    }

    Stop-VerifiedBridgePair -Pair $firstPair
    $cacheSignal = Join-Path $reportRoot 'signal-cache'
    $cachePair = Start-BridgePair -Epoch 'epoch-local-selftest-2' -Suffix 'cache' `
        -SignalDirectory $cacheSignal -HostCache $hostCache -ControllerCache $controllerCache
    $cacheStatus = Wait-BridgePair -Pair $cachePair
    $result.cache_pair_connected =
        [bool]$cacheStatus.Host.channel_open -and [bool]$cacheStatus.Controller.channel_open
    $result.host_cache_hit = [bool]$cacheStatus.Host.route_cache_hit
    $result.controller_cache_hit = [bool]$cacheStatus.Controller.route_cache_hit
    $result.passed = $result.first_pair_connected -and
        $result.first_host_description_role_matches -and
        $result.first_controller_description_role_matches -and
        $result.first_host_has_ice_ufrag -and $result.first_controller_has_ice_ufrag -and
        $result.first_host_has_ice_pwd -and $result.first_controller_has_ice_pwd -and
        $result.first_host_has_fingerprint -and $result.first_controller_has_fingerprint -and
        $result.first_host_signal_publish_total -gt 0 -and
        $result.first_controller_signal_publish_total -gt 0 -and
        $result.remote_bridge_roundtrip -and
        $result.cache_pair_connected -and
        $result.host_cache_hit -and
        $result.controller_cache_hit -and
        (-not $RequireRedClawRelay -or
            ($result.redclaw_relay_ok -and $result.remote_redaction_ok))
} catch {
    $result.error = $_.Exception.Message
} finally {
    if (-not $KeepProcesses) {
        foreach ($processId in $processIds) {
            $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
            if ($null -ne $process -and $process.ProcessName -eq 'redclaw_debug_bridge') {
                Stop-Process -Id $processId -Force
            }
        }
    }
    Remove-Item -LiteralPath $passphrasePath -Force -ErrorAction SilentlyContinue
    $resultPath = Join-Path $reportRoot 'result.json'
    $result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $resultPath -Encoding utf8
    $result | ConvertTo-Json -Depth 10
}

if (-not $result.passed) {
    exit 1
}
