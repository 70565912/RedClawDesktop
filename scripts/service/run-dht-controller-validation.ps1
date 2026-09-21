param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9]{8}$')]
    [string]$SessionCode,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$RuntimeExe = "",

    [Parameter(Mandatory = $true)]
    [string]$ConnectionCredentialFile,

    [string]$NetworkBindAddress = "",

    [string]$ReportRoot = "build\reports",

    [string[]]$IceServer = @("stun:stun.l.google.com:19302"),

    [string[]]$DhtBootstrap = @(
        "router.bittorrent.com:6881",
        "dht.transmissionbt.com:6881",
        "dht.libtorrent.org:25401",
        "router.utorrent.com:6881"
    ),

    [ValidateRange(0, 65535)]
    [int]$DhtListenPort = 0,

    [ValidateRange(1, 65535)]
    [int]$IceUdpPort = 55000,

    [ValidateRange(250, 60000)]
    [int]$DhtPollIntervalMs = 1000,

    [ValidateRange(1000, 120000)]
    [int]$DhtPublishRetryMs = 1000,

    [ValidateRange(10, 3600)]
    [int]$RunSeconds = 240,

    [ValidateRange(0, 3600)]
    [int]$WaitSeconds = 0,

    [ValidateRange(0, 600)]
    [int]$StopAfterConnectedSeconds = 20,

    [bool]$EnableIceTcp = $true,

    [bool]$EnablePortMapping = $true,

    [switch]$DisableIpv6Candidates,

    [ValidateRange(500, 30000)]
    [int]$BootstrapDnsTimeoutMs = 5000,

    [switch]$SkipBootstrapDoh,

    [switch]$SkipBootstrapReadiness,

    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $candidate = Resolve-Path (Join-Path $PSScriptRoot "..\..")
    return $candidate.Path
}

function Get-RedactedSessionCode {
    param([string]$Code)

    if ([string]::IsNullOrWhiteSpace($Code) -or $Code.Length -lt 4) {
        return "<redacted>"
    }

    return ($Code.Substring(0, 4) + "****")
}

function Get-TextIfExists {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        return ""
    }

    return Get-Content -Path $Path -Raw
}

function Test-ConnectedFromLog {
    param([string]$Text)

    return $Text -match "desktop stream data channel opened" -or $Text -match "connected=true"
}

function Get-LastMatchingLine {
    param(
        [string]$Text,
        [string]$Pattern
    )

    if ([string]::IsNullOrEmpty($Text)) {
        return ""
    }

    $matches = [regex]::Matches($Text, ".*$Pattern.*")
    if ($matches.Count -eq 0) {
        return ""
    }

    return $matches[$matches.Count - 1].Value.Trim()
}

function Write-SummaryJson {
    param(
        [string]$Path,
        [hashtable]$Summary
    )

    $Summary | ConvertTo-Json -Depth 5 | Set-Content -Path $Path -Encoding UTF8
}

function Invoke-BootstrapReadiness {
    param(
        [string]$OutputPath,
        [string[]]$BootstrapNodes,
        [int]$TimeoutMs,
        [bool]$SkipDohValue,
        [bool]$SkipReadinessValue,
        [bool]$DryRunValue,
        [string]$LogPrefix
    )

    $result = [ordered]@{
        enabled = (-not $SkipReadinessValue)
        status = if ($SkipReadinessValue) { "skipped" } else { "not_run" }
        output_path = $OutputPath
        exit_code = $null
        usable_nodes = 0
        suspicious_nodes = 0
        failed_nodes = 0
        blocked = $false
        error = ""
    }

    if ($SkipReadinessValue) {
        return [pscustomobject]$result
    }

    $scriptPath = Join-Path $PSScriptRoot "test-dht-bootstrap-readiness.ps1"
    if (-not (Test-Path $scriptPath)) {
        $result["status"] = "script_missing"
        $result["error"] = "Bootstrap readiness script not found: $scriptPath"
        return [pscustomobject]$result
    }

    try {
        Write-Host "[$LogPrefix] bootstrap readiness: $OutputPath"
        $parameters = @{
            DhtBootstrap = $BootstrapNodes
            OutputPath = $OutputPath
            DnsTimeoutMs = $TimeoutMs
        }
        if ($SkipDohValue) {
            $parameters.SkipDoh = $true
        }
        if ($DryRunValue) {
            $parameters.DryRun = $true
        }

        & $scriptPath @parameters
        $exitCode = $LASTEXITCODE
        $result["exit_code"] = $exitCode

        if (Test-Path $OutputPath) {
            $readinessSummary = Get-Content -Path $OutputPath -Raw | ConvertFrom-Json
            $result["status"] = [string]$readinessSummary.overall_status
            $result["usable_nodes"] = [int]$readinessSummary.counts.usable_nodes
            $result["suspicious_nodes"] = [int]$readinessSummary.counts.suspicious_nodes
            $result["failed_nodes"] = [int]$readinessSummary.counts.failed_nodes
        } else {
            $result["status"] = "missing_summary"
        }

        if ($exitCode -eq 2 -or $result["status"] -eq "blocked_no_usable_bootstrap") {
            $result["blocked"] = $true
        }
    } catch {
        $result["status"] = "preflight_error"
        $result["error"] = $_.Exception.Message
    }

    return [pscustomobject]$result
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

if ($WaitSeconds -le 0) {
    $WaitSeconds = $RunSeconds + 30
}

if ([string]::IsNullOrWhiteSpace($RuntimeExe)) {
    $RuntimeExe = Join-Path $repoRoot ("release\{0}\redclaw_desktop.exe" -f $Configuration)
}

if (-not (Test-Path $RuntimeExe)) {
    throw "Runtime executable not found: $RuntimeExe. If the build tree is already configured, build and publish first with .\build.ps1 -Configuration $Configuration -SkipConfigure -Target redclaw_desktop -Parallel 1."
}

$resolvedRuntimeExe = (Resolve-Path $RuntimeExe).Path

if (-not [string]::IsNullOrWhiteSpace($NetworkBindAddress)) {
    $parsedNetworkAddress = $null
    if (-not [System.Net.IPAddress]::TryParse($NetworkBindAddress, [ref]$parsedNetworkAddress) -or
        $parsedNetworkAddress.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
        throw "NetworkBindAddress must be an IPv4 address."
    }
}

$resolvedReportRoot = Join-Path $repoRoot $ReportRoot
$timestamp = "{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss_fff"), ([guid]::NewGuid().ToString("N").Substring(0, 6))
$reportDirectory = Join-Path $resolvedReportRoot ("dht-controller-validation-{0}" -f $timestamp)
$stdoutPath = Join-Path $reportDirectory "controller.out.log"
$stderrPath = Join-Path $reportDirectory "controller.err.log"
$summaryPath = Join-Path $reportDirectory "summary.json"
$bootstrapReadinessPath = Join-Path $reportDirectory "bootstrap-readiness.json"

$arguments = @(
    "--cli",
    "--role", "controller",
    "--connection-credential-file", (Resolve-Path -LiteralPath $ConnectionCredentialFile).Path,
    "--signal-transport", "dht",
    "--session-code", $SessionCode,
    "--signal-timeout-seconds", [string]$RunSeconds,
    "--run-seconds", [string]$RunSeconds,
    "--dht-poll-interval-ms", [string]$DhtPollIntervalMs,
    "--dht-publish-retry-ms", [string]$DhtPublishRetryMs,
    "--ice-udp-port", [string]$IceUdpPort
)

foreach ($server in $IceServer) {
    if (-not [string]::IsNullOrWhiteSpace($server)) {
        $arguments += @("--ice-server", $server)
    }
}

foreach ($node in $DhtBootstrap) {
    if (-not [string]::IsNullOrWhiteSpace($node)) {
        $arguments += @("--dht-bootstrap", $node)
    }
}

if ($DhtListenPort -gt 0) {
    $arguments += @("--dht-listen-port", [string]$DhtListenPort)
}

if (-not [string]::IsNullOrWhiteSpace($NetworkBindAddress)) {
    $arguments += @("--network-bind-address", $NetworkBindAddress)
}

if ($EnableIceTcp) {
    $arguments += "--enable-ice-tcp"
}

if ($EnablePortMapping) {
    $arguments += "--enable-port-mapping"
}

if ($DisableIpv6Candidates) {
    $arguments += "--disable-ipv6-candidates"
}

$redactedArguments = @()
for ($index = 0; $index -lt $arguments.Count; ++$index) {
    if ($arguments[$index] -eq "--session-code" -and $index + 1 -lt $arguments.Count) {
        $redactedArguments += "--session-code"
        $redactedArguments += (Get-RedactedSessionCode -Code $arguments[$index + 1])
        ++$index
        continue
    }

    $redactedArguments += $arguments[$index]
}

New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null

$bootstrapReadiness = Invoke-BootstrapReadiness `
    -OutputPath $bootstrapReadinessPath `
    -BootstrapNodes $DhtBootstrap `
    -TimeoutMs $BootstrapDnsTimeoutMs `
    -SkipDohValue ([bool]$SkipBootstrapDoh) `
    -SkipReadinessValue ([bool]$SkipBootstrapReadiness) `
    -DryRunValue ([bool]$DryRun) `
    -LogPrefix "dht-controller"

$initialSummary = @{
    status = if ($DryRun) { "dry_run" } else { "running" }
    role = "controller"
    session_code_redacted = Get-RedactedSessionCode -Code $SessionCode
    runtime_exe = $resolvedRuntimeExe
    network_bind_address = $(if ([string]::IsNullOrWhiteSpace($NetworkBindAddress)) { "auto" } else { $NetworkBindAddress })
    ice_udp_port = $IceUdpPort
    report_directory = $reportDirectory
    stdout = $stdoutPath
    stderr = $stderrPath
    arguments_redacted = $redactedArguments
    bootstrap_readiness = $bootstrapReadiness
    started_at = (Get-Date).ToString("o")
}
Write-SummaryJson -Path $summaryPath -Summary $initialSummary

Write-Host "[dht-controller] report: $reportDirectory"
Write-Host "[dht-controller] runtime: $resolvedRuntimeExe"
Write-Host "[dht-controller] session: $(Get-RedactedSessionCode -Code $SessionCode)"

if ($bootstrapReadiness.blocked -and -not $DryRun) {
    $blockedSummary = $initialSummary.Clone()
    $blockedSummary["status"] = "bootstrap_blocked"
    $blockedSummary["finished_at"] = (Get-Date).ToString("o")
    Write-SummaryJson -Path $summaryPath -Summary $blockedSummary
    Write-Host "[dht-controller] bootstrap readiness blocked runtime start: $($bootstrapReadiness.status)"
    Write-Host "[dht-controller] summary: $summaryPath"
    exit 1
}

if ($DryRun) {
    Write-Host "[dht-controller] dry run only; runtime was not started."
    Write-Host "[dht-controller] redacted args: $($redactedArguments -join ' ')"
    exit 0
}

$controllerProc = $null
$connectedAt = $null
$timedOut = $false
$stoppedAfterSuccess = $false

try {
    $controllerProc = Start-Process `
        -FilePath $resolvedRuntimeExe `
        -ArgumentList $arguments `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -PassThru `
        -WindowStyle Hidden

    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($controllerProc.HasExited) {
            break
        }

        $stdoutText = Get-TextIfExists -Path $stdoutPath
        if ($null -eq $connectedAt -and (Test-ConnectedFromLog -Text $stdoutText)) {
            $connectedAt = Get-Date
            Write-Host "[dht-controller] connected signal observed; holding for $StopAfterConnectedSeconds second(s)."
        }

        if ($null -ne $connectedAt -and $StopAfterConnectedSeconds -gt 0) {
            if (((Get-Date) - $connectedAt).TotalSeconds -ge $StopAfterConnectedSeconds) {
                Stop-Process -Id $controllerProc.Id -Force
                $stoppedAfterSuccess = $true
                break
            }
        }

        Start-Sleep -Seconds 1
    }

    if (-not $controllerProc.HasExited -and -not $stoppedAfterSuccess) {
        $timedOut = $true
        Stop-Process -Id $controllerProc.Id -Force
    }
} finally {
    if ($null -ne $controllerProc -and -not $controllerProc.HasExited) {
        Stop-Process -Id $controllerProc.Id -Force
    }
}

$finalStdout = Get-TextIfExists -Path $stdoutPath
$finalStderr = Get-TextIfExists -Path $stderrPath
$connected = Test-ConnectedFromLog -Text $finalStdout
$status = if ($connected) {
    "success_connected"
} elseif ($timedOut) {
    "timeout_before_success"
} elseif ($finalStderr -match "Runtime ICE state failed") {
    "ice_failed"
} elseif ($finalStderr -match "Runtime signaling timeout") {
    "signaling_timeout"
} else {
    "exit_before_success"
}

$exitCode = $null
if ($null -ne $controllerProc) {
    $exitCode = $controllerProc.ExitCode
}

$summary = @{
    status = $status
    role = "controller"
    session_code_redacted = Get-RedactedSessionCode -Code $SessionCode
    runtime_exe = $resolvedRuntimeExe
    network_bind_address = $(if ([string]::IsNullOrWhiteSpace($NetworkBindAddress)) { "auto" } else { $NetworkBindAddress })
    report_directory = $reportDirectory
    stdout = $stdoutPath
    stderr = $stderrPath
    arguments_redacted = $redactedArguments
    bootstrap_readiness = $bootstrapReadiness
    started_at = $initialSummary.started_at
    finished_at = (Get-Date).ToString("o")
    exit_code = $exitCode
    timed_out = $timedOut
    stopped_after_success = $stoppedAfterSuccess
    connected = $connected
    data_channel_opened = ($finalStdout -match "desktop stream data channel opened")
    remote_description_applied = ($finalStdout -match "Runtime remote description applied role=controller")
    last_runtime_state = Get-LastMatchingLine -Text $finalStdout -Pattern "Runtime state role=controller"
    last_candidate_stats = Get-LastMatchingLine -Text $finalStdout -Pattern "Runtime candidate stats role=controller"
    last_dht_stats = Get-LastMatchingLine -Text $finalStdout -Pattern "Runtime DHT stats role=controller"
    last_dht_backend_diagnostics = Get-LastMatchingLine -Text $finalStdout -Pattern "Runtime DHT backend diagnostics role=controller"
    stderr_tail = (($finalStderr -split "`r?`n") | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Last 10)
}

Write-SummaryJson -Path $summaryPath -Summary $summary

Write-Host "[dht-controller] status: $status"
Write-Host "[dht-controller] summary: $summaryPath"
if ($summary.last_runtime_state) {
    Write-Host "[dht-controller] $($summary.last_runtime_state)"
}
if ($summary.last_candidate_stats) {
    Write-Host "[dht-controller] $($summary.last_candidate_stats)"
}
if ($summary.last_dht_stats) {
    Write-Host "[dht-controller] $($summary.last_dht_stats)"
}

if ($status -ne "success_connected") {
    exit 1
}

exit 0
