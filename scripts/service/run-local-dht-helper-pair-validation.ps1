param(
    [ValidatePattern('^[A-Za-z0-9]{8}$')]
    [string]$SessionCode = "",

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$RuntimeExe = "",

    [string]$NetworkBindAddress = "",

    [string]$ReportRoot = "build\reports",

    [string[]]$IceServer = @("stun:stun.l.google.com:19302"),

    [string[]]$DhtBootstrap = @(
        "router.bittorrent.com:6881",
        "dht.transmissionbt.com:6881",
        "dht.libtorrent.org:25401",
        "router.utorrent.com:6881"
    ),

    [ValidateRange(250, 60000)]
    [int]$DhtPollIntervalMs = 1000,

    [ValidateRange(1000, 120000)]
    [int]$DhtPublishRetryMs = 1000,

    [ValidateRange(10, 3600)]
    [int]$RunSeconds = 100,

    [ValidateRange(0, 3600)]
    [int]$WaitSeconds = 0,

    [ValidateRange(0, 600)]
    [int]$StopAfterConnectedSeconds = 20,

    [ValidateRange(0, 120)]
    [int]$ControllerStartDelaySeconds = 4,

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

function New-SessionCode {
    $alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789".ToCharArray()
    $builder = New-Object System.Text.StringBuilder

    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $buffer = New-Object byte[] 1
    $limit = [int](256 - (256 % $alphabet.Length))
    try {
        while ($builder.Length -lt 8) {
            $rng.GetBytes($buffer)
            $value = [int]$buffer[0]
            if ($value -ge $limit) {
                continue
            }

            $charIndex = $value % $alphabet.Length
            [void]$builder.Append($alphabet[$charIndex])
        }
    } finally {
        $rng.Dispose()
    }

    return $builder.ToString()
}

function Get-RedactedSessionCode {
    param([string]$Code)

    if ([string]::IsNullOrWhiteSpace($Code) -or $Code.Length -lt 4) {
        return "<redacted>"
    }

    return ($Code.Substring(0, 4) + "****")
}

function Invoke-ValidationScript {
    param(
        [string]$Root,
        [string]$ScriptPath,
        [string]$Code,
        [string]$ConfigurationName,
        [string]$RuntimePath,
        [string]$NetworkBindAddressValue,
        [string]$ReportRootPath,
        [string[]]$IceServers,
        [string[]]$DhtBootstrapNodes,
        [int]$PollIntervalMs,
        [int]$PublishRetryMs,
        [int]$RuntimeSeconds,
        [int]$WaitSecondsValue,
        [int]$StopAfterConnectedSecondsValue,
        [bool]$EnableIceTcpValue,
        [bool]$EnablePortMappingValue,
        [bool]$DisableIpv6CandidatesValue,
        [int]$BootstrapDnsTimeoutMsValue,
        [bool]$SkipBootstrapDohValue,
        [bool]$SkipBootstrapReadinessValue,
        [bool]$DryRunValue
    )

    Set-Location $Root
    $parameters = @{
        SessionCode = $Code
        Configuration = $ConfigurationName
        ReportRoot = $ReportRootPath
        IceServer = $IceServers
        DhtBootstrap = $DhtBootstrapNodes
        DhtPollIntervalMs = $PollIntervalMs
        DhtPublishRetryMs = $PublishRetryMs
        RunSeconds = $RuntimeSeconds
        WaitSeconds = $WaitSecondsValue
        StopAfterConnectedSeconds = $StopAfterConnectedSecondsValue
        BootstrapDnsTimeoutMs = $BootstrapDnsTimeoutMsValue
    }

    if (-not [string]::IsNullOrWhiteSpace($RuntimePath)) {
        $parameters.RuntimeExe = $RuntimePath
    }
    if (-not [string]::IsNullOrWhiteSpace($NetworkBindAddressValue)) {
        $parameters.NetworkBindAddress = $NetworkBindAddressValue
    }
    $parameters.EnableIceTcp = $EnableIceTcpValue
    $parameters.EnablePortMapping = $EnablePortMappingValue
    if ($DisableIpv6CandidatesValue) {
        $parameters.DisableIpv6Candidates = $true
    }
    if ($SkipBootstrapDohValue) {
        $parameters.SkipBootstrapDoh = $true
    }
    if ($SkipBootstrapReadinessValue) {
        $parameters.SkipBootstrapReadiness = $true
    }
    if ($DryRunValue) {
        $parameters.DryRun = $true
    }

    & $ScriptPath @parameters
    exit $LASTEXITCODE
}

function Get-ReportDirectoryFromOutput {
    param(
        [object[]]$Output,
        [string]$Prefix
    )

    foreach ($line in $Output) {
        $text = [string]$line
        if ($line -is [System.Management.Automation.InformationRecord]) {
            $text = [string]$line.MessageData
        }

        $match = [regex]::Match($text, "^\[$Prefix\] report: (.+)$")
        if ($match.Success) {
            return $match.Groups[1].Value.Trim()
        }
    }

    return ""
}

function Get-LatestReportDirectory {
    param(
        [string]$Root,
        [string]$Prefix,
        [datetime]$StartedAfter
    )

    $base = Join-Path $Root $ReportRoot
    if (-not (Test-Path $base)) {
        return ""
    }

    $match = Get-ChildItem -Path $base -Directory -Filter ("{0}-*" -f $Prefix) -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $StartedAfter.AddSeconds(-2) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if ($null -eq $match) {
        return ""
    }

    return $match.FullName
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

if ([string]::IsNullOrWhiteSpace($SessionCode)) {
    $SessionCode = New-SessionCode
}

if ($WaitSeconds -le 0) {
    $WaitSeconds = $RunSeconds + 30
}

$hostScript = Join-Path $PSScriptRoot "run-dht-host-validation.ps1"
$controllerScript = Join-Path $PSScriptRoot "run-dht-controller-validation.ps1"
$summarizerScript = Join-Path $PSScriptRoot "summarize-dht-validation-report.ps1"

foreach ($scriptPath in @($hostScript, $controllerScript, $summarizerScript)) {
    if (-not (Test-Path $scriptPath)) {
        throw "Required script not found: $scriptPath"
    }
}

$timestamp = "{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss_fff"), ([guid]::NewGuid().ToString("N").Substring(0, 6))
$reportRootAbsolute = Join-Path $repoRoot $ReportRoot
$summaryPath = Join-Path $reportRootAbsolute ("dht-helper-pair-summary-{0}.json" -f $timestamp)

Write-Host "[dht-pair] session: $(Get-RedactedSessionCode -Code $SessionCode)"
Write-Host "[dht-pair] mode: $(if ($DryRun) { 'dry-run' } else { 'live' })"
Write-Host "[dht-pair] network_bind_address: $(if ([string]::IsNullOrWhiteSpace($NetworkBindAddress)) { 'auto' } else { $NetworkBindAddress })"

$startedAt = Get-Date
$hostJob = Start-Job -Name "redclaw-dht-host-helper" -ScriptBlock ${function:Invoke-ValidationScript} -ArgumentList @(
    $repoRoot,
    $hostScript,
    $SessionCode,
    $Configuration,
    $RuntimeExe,
    $NetworkBindAddress,
    $ReportRoot,
    $IceServer,
    $DhtBootstrap,
    $DhtPollIntervalMs,
    $DhtPublishRetryMs,
    $RunSeconds,
    $WaitSeconds,
    $StopAfterConnectedSeconds,
    [bool]$EnableIceTcp,
    [bool]$EnablePortMapping,
    [bool]$DisableIpv6Candidates,
    $BootstrapDnsTimeoutMs,
    [bool]$SkipBootstrapDoh,
    [bool]$SkipBootstrapReadiness,
    [bool]$DryRun
)

Start-Sleep -Seconds $ControllerStartDelaySeconds

$controllerJob = Start-Job -Name "redclaw-dht-controller-helper" -ScriptBlock ${function:Invoke-ValidationScript} -ArgumentList @(
    $repoRoot,
    $controllerScript,
    $SessionCode,
    $Configuration,
    $RuntimeExe,
    $NetworkBindAddress,
    $ReportRoot,
    $IceServer,
    $DhtBootstrap,
    $DhtPollIntervalMs,
    $DhtPublishRetryMs,
    $RunSeconds,
    $WaitSeconds,
    $StopAfterConnectedSeconds,
    [bool]$EnableIceTcp,
    [bool]$EnablePortMapping,
    [bool]$DisableIpv6Candidates,
    $BootstrapDnsTimeoutMs,
    [bool]$SkipBootstrapDoh,
    [bool]$SkipBootstrapReadiness,
    [bool]$DryRun
)

$jobs = @($hostJob, $controllerJob)
$jobTimeoutSeconds = $WaitSeconds + $ControllerStartDelaySeconds + 60
Wait-Job -Job $jobs -Timeout $jobTimeoutSeconds | Out-Null

$outputsByJob = @{}
foreach ($job in $jobs) {
    Write-Host "==== JOB $($job.Name) STATE=$($job.State) ===="
    $jobOutput = @(Receive-Job -Job $job -Keep)
    $outputsByJob[$job.Name] = $jobOutput
    foreach ($line in $jobOutput) {
        Write-Host $line
    }
}

$runningJobs = @($jobs | Where-Object { $_.State -eq "Running" })
if ($runningJobs.Count -gt 0) {
    $runningJobs | Stop-Job
    Write-Host "[dht-pair] stopped running jobs: $($runningJobs.Name -join ', ')"
}

$failedJobs = @($jobs | Where-Object { $_.State -ne "Completed" })
$hostReport = Get-ReportDirectoryFromOutput -Output $outputsByJob["redclaw-dht-host-helper"] -Prefix "dht-host"
$controllerReport = Get-ReportDirectoryFromOutput -Output $outputsByJob["redclaw-dht-controller-helper"] -Prefix "dht-controller"
if ([string]::IsNullOrWhiteSpace($hostReport)) {
    $hostReport = Get-LatestReportDirectory -Root $repoRoot -Prefix "dht-host-validation" -StartedAfter $startedAt
}
if ([string]::IsNullOrWhiteSpace($controllerReport)) {
    $controllerReport = Get-LatestReportDirectory -Root $repoRoot -Prefix "dht-controller-validation" -StartedAfter $startedAt
}

$jobs | Remove-Job -Force

if ([string]::IsNullOrWhiteSpace($hostReport) -or [string]::IsNullOrWhiteSpace($controllerReport)) {
    throw "Failed to locate both helper report directories. host='$hostReport' controller='$controllerReport'"
}

Write-Host "[dht-pair] host report: $hostReport"
Write-Host "[dht-pair] controller report: $controllerReport"

& $summarizerScript -ReportDirectory @($hostReport, $controllerReport) -OutputPath $summaryPath
$summaryExitCode = $LASTEXITCODE
Write-Host "[dht-pair] summary: $summaryPath"

if ($failedJobs.Count -gt 0) {
    exit 1
}

exit $summaryExitCode
