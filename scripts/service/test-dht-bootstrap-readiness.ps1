param(
    [string[]]$DhtBootstrap = @(
        "router.bittorrent.com:6881",
        "dht.transmissionbt.com:6881",
        "dht.libtorrent.org:25401",
        "router.utorrent.com:6881"
    ),

    [string]$ReportRoot = "build\reports",

    [string]$OutputPath = "",

    [ValidateRange(500, 30000)]
    [int]$DnsTimeoutMs = 5000,

    [string]$DohEndpoint = "https://cloudflare-dns.com/dns-query",

    [switch]$SkipDoh,

    [switch]$Json,

    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $candidate = Resolve-Path (Join-Path $PSScriptRoot "..\..")
    return $candidate.Path
}

function Split-BootstrapNode {
    param([string]$Value)

    $trimmed = ([string]$Value).Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return [pscustomobject]@{
            input = $Value
            host = ""
            port = 0
            valid = $false
            error = "empty bootstrap node"
        }
    }

    $nodeHost = $trimmed
    $port = 0
    if ($trimmed.StartsWith("[")) {
        $endBracket = $trimmed.IndexOf("]")
        if ($endBracket -gt 1) {
            $nodeHost = $trimmed.Substring(1, $endBracket - 1)
            $suffix = $trimmed.Substring($endBracket + 1)
            if ($suffix.StartsWith(":")) {
                $parsedPort = 0
                if ([int]::TryParse($suffix.Substring(1), [ref]$parsedPort)) {
                    $port = $parsedPort
                }
            }
        }
    } else {
        $lastColon = $trimmed.LastIndexOf(":")
        if ($lastColon -gt 0 -and $lastColon -lt ($trimmed.Length - 1)) {
            $parsedPort = 0
            if ([int]::TryParse($trimmed.Substring($lastColon + 1), [ref]$parsedPort)) {
                $nodeHost = $trimmed.Substring(0, $lastColon)
                $port = $parsedPort
            }
        }
    }

    $valid = -not [string]::IsNullOrWhiteSpace($nodeHost)
    if ($port -lt 0 -or $port -gt 65535) {
        $valid = $false
    }

    return [pscustomobject]@{
        input = $trimmed
        host = $nodeHost
        port = $port
        valid = $valid
        error = if ($valid) { "" } else { "invalid bootstrap node" }
    }
}

function Get-KnownBootstrapFallbacks {
    param([string]$Node)

    $lowered = ([string]$Node).Trim().ToLowerInvariant()
    if ($lowered -eq "router.bittorrent.com:6881") {
        return @("67.215.246.10:6881")
    }
    if ($lowered -eq "dht.transmissionbt.com:6881") {
        return @(
            "87.98.162.88:6881",
            "212.129.33.59:6881"
        )
    }
    if ($lowered -eq "dht.libtorrent.org:25401") {
        return @("185.157.221.247:25401")
    }
    if ($lowered -eq "router.utorrent.com:6881") {
        return @("82.221.103.244:6881")
    }

    return @()
}

function Get-IpSuspicionReasons {
    param([string]$Value)

    $ip = [System.Net.IPAddress]::None
    if (-not [System.Net.IPAddress]::TryParse($Value, [ref]$ip)) {
        return @("not_an_ip")
    }

    $reasons = @()
    if ($ip.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork) {
        $bytes = $ip.GetAddressBytes()
        $a = [int]$bytes[0]
        $b = [int]$bytes[1]
        $c = [int]$bytes[2]
        $d = [int]$bytes[3]

        if ($a -eq 0) {
            $reasons += "ipv4_this_network"
        }
        if ($a -eq 10) {
            $reasons += "ipv4_private_10_8"
        }
        if ($a -eq 100 -and $b -ge 64 -and $b -le 127) {
            $reasons += "ipv4_carrier_nat_100_64_10"
        }
        if ($a -eq 127) {
            $reasons += "ipv4_loopback_127_8"
        }
        if ($a -eq 169 -and $b -eq 254) {
            $reasons += "ipv4_link_local_169_254_16"
        }
        if ($a -eq 172 -and $b -ge 16 -and $b -le 31) {
            $reasons += "ipv4_private_172_16_12"
        }
        if ($a -eq 192 -and $b -eq 168) {
            $reasons += "ipv4_private_192_168_16"
        }
        if ($a -eq 198 -and ($b -eq 18 -or $b -eq 19)) {
            $reasons += "ipv4_benchmark_or_fake_ip_198_18_15"
        }
        if ($a -eq 28) {
            $reasons += "ipv4_fake_ip_pool_observed_28_8"
        }
        if ($a -ge 224) {
            $reasons += "ipv4_multicast_or_reserved_224_plus"
        }
        if ($a -eq 255 -and $b -eq 255 -and $c -eq 255 -and $d -eq 255) {
            $reasons += "ipv4_broadcast"
        }
    } elseif ($ip.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetworkV6) {
        if ([System.Net.IPAddress]::IPv6Loopback.Equals($ip)) {
            $reasons += "ipv6_loopback"
        }
        if ($ip.IsIPv6LinkLocal) {
            $reasons += "ipv6_link_local"
        }
        if ($ip.IsIPv6Multicast) {
            $reasons += "ipv6_multicast"
        }
        if ([System.Net.IPAddress]::IPv6None.Equals($ip) -or [System.Net.IPAddress]::IPv6Any.Equals($ip)) {
            $reasons += "ipv6_unspecified"
        }
    }

    return $reasons
}

function Resolve-HostAddressesWithTimeout {
    param(
        [string]$HostName,
        [int]$TimeoutMs
    )

    try {
        $ip = [System.Net.IPAddress]::None
        if ([System.Net.IPAddress]::TryParse($HostName, [ref]$ip)) {
            return [pscustomobject]@{
                status = "literal"
                addresses = @($ip.ToString())
                error = ""
            }
        }

        $task = [System.Net.Dns]::GetHostAddressesAsync($HostName)
        if (-not $task.Wait($TimeoutMs)) {
            return [pscustomobject]@{
                status = "timeout"
                addresses = @()
                error = "DNS lookup timed out after $TimeoutMs ms"
            }
        }

        $addresses = @()
        foreach ($address in $task.Result) {
            $addresses += $address.ToString()
        }

        return [pscustomobject]@{
            status = if ($addresses.Count -gt 0) { "resolved" } else { "empty" }
            addresses = $addresses
            error = ""
        }
    } catch {
        return [pscustomobject]@{
            status = "error"
            addresses = @()
            error = $_.Exception.Message
        }
    }
}

function Invoke-DohQuery {
    param(
        [string]$HostName,
        [string]$Endpoint,
        [int]$TimeoutMs
    )

    try {
        $ip = [System.Net.IPAddress]::None
        if ([System.Net.IPAddress]::TryParse($HostName, [ref]$ip)) {
            return [pscustomobject]@{
                status = "literal"
                addresses = @($ip.ToString())
                error = ""
            }
        }

        $timeoutSeconds = [Math]::Max(1, [int][Math]::Ceiling($TimeoutMs / 1000.0))
        $allAddresses = @()
        foreach ($recordType in @("A", "AAAA")) {
            $uri = "{0}?name={1}&type={2}" -f $Endpoint, [System.Uri]::EscapeDataString($HostName), $recordType
            $response = Invoke-RestMethod -Uri $uri -Headers @{ accept = "application/dns-json" } -TimeoutSec $timeoutSeconds
            if ($null -eq $response.Answer) {
                continue
            }

            foreach ($answer in $response.Answer) {
                $address = [string]$answer.data
                $parsedIp = [System.Net.IPAddress]::None
                if ([System.Net.IPAddress]::TryParse($address, [ref]$parsedIp)) {
                    $allAddresses += $parsedIp.ToString()
                }
            }
        }

        $unique = @($allAddresses | Sort-Object -Unique)
        return [pscustomobject]@{
            status = if ($unique.Count -gt 0) { "resolved" } else { "empty" }
            addresses = $unique
            error = ""
        }
    } catch {
        return [pscustomobject]@{
            status = "error"
            addresses = @()
            error = $_.Exception.Message
        }
    }
}

function ConvertTo-AddressDiagnostics {
    param([string[]]$Addresses)

    $items = @()
    foreach ($address in $Addresses) {
        $reasons = @(Get-IpSuspicionReasons -Value $address)
        $items += [pscustomobject]@{
            address = $address
            suspicious = $reasons.Count -gt 0
            reasons = $reasons
        }
    }

    return $items
}

function Get-RoutableAddressCount {
    param([object[]]$AddressDiagnostics)

    $count = 0
    foreach ($item in $AddressDiagnostics) {
        if (-not [bool]$item.suspicious) {
            ++$count
        }
    }

    return $count
}

function Get-NodeStatus {
    param(
        [object]$LocalDns,
        [object[]]$LocalDiagnostics,
        [string[]]$Fallbacks,
        [bool]$DryRunMode
    )

    if ($DryRunMode) {
        return "dry_run"
    }

    $routableCount = Get-RoutableAddressCount -AddressDiagnostics $LocalDiagnostics
    if ($routableCount -gt 0) {
        return "local_dns_ready"
    }

    if ($LocalDns.addresses.Count -gt 0 -and $Fallbacks.Count -gt 0) {
        return "suspicious_local_dns_with_cached_fallback"
    }

    if ($LocalDns.addresses.Count -eq 0 -and $Fallbacks.Count -gt 0) {
        return "cached_fallback_only"
    }

    if ($LocalDns.addresses.Count -gt 0) {
        return "suspicious_local_dns_without_fallback"
    }

    return "resolution_failed"
}

function Write-SummaryJson {
    param(
        [string]$Path,
        [object]$Summary
    )

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    $Summary | ConvertTo-Json -Depth 8 | Set-Content -Path $Path -Encoding UTF8
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $reportRootPath = Join-Path $repoRoot $ReportRoot
    $runId = "{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss_fff"), ([guid]::NewGuid().ToString("N").Substring(0, 6))
    $reportDirectory = Join-Path $reportRootPath ("dht-bootstrap-readiness-{0}" -f $runId)
    $OutputPath = Join-Path $reportDirectory "summary.json"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath = Join-Path $repoRoot $OutputPath
}

$nodeReports = @()
foreach ($node in $DhtBootstrap) {
    $parsed = Split-BootstrapNode -Value $node
    $fallbacks = @(Get-KnownBootstrapFallbacks -Node $parsed.input)
    $localDns = [pscustomobject]@{
        status = "skipped"
        addresses = @()
        error = ""
    }
    $doh = [pscustomobject]@{
        status = if ($SkipDoh) { "skipped" } else { "not_run" }
        addresses = @()
        error = ""
    }

    if (-not $DryRun -and $parsed.valid) {
        $localDns = Resolve-HostAddressesWithTimeout -HostName $parsed.host -TimeoutMs $DnsTimeoutMs
        if (-not $SkipDoh) {
            $doh = Invoke-DohQuery -HostName $parsed.host -Endpoint $DohEndpoint -TimeoutMs $DnsTimeoutMs
        }
    }

    $localDiagnostics = @(ConvertTo-AddressDiagnostics -Addresses $localDns.addresses)
    $dohDiagnostics = @(ConvertTo-AddressDiagnostics -Addresses $doh.addresses)
    $nodeStatus = Get-NodeStatus -LocalDns $localDns -LocalDiagnostics $localDiagnostics -Fallbacks $fallbacks -DryRunMode ([bool]$DryRun)

    $nodeReports += [pscustomobject]@{
        input = $parsed.input
        host = $parsed.host
        port = $parsed.port
        valid = $parsed.valid
        parse_error = $parsed.error
        status = if ($parsed.valid) { $nodeStatus } else { "invalid" }
        local_dns = [pscustomobject]@{
            status = $localDns.status
            addresses = $localDiagnostics
            error = $localDns.error
        }
        doh = [pscustomobject]@{
            endpoint = if ($SkipDoh) { "" } else { $DohEndpoint }
            status = $doh.status
            addresses = $dohDiagnostics
            error = $doh.error
        }
        cached_runtime_fallbacks = $fallbacks
    }
}

$usableNodes = 0
$suspiciousNodes = 0
$failedNodes = 0
foreach ($nodeReport in $nodeReports) {
    if ($nodeReport.status -in @("local_dns_ready", "suspicious_local_dns_with_cached_fallback", "cached_fallback_only")) {
        ++$usableNodes
    }
    if ($nodeReport.status -in @("suspicious_local_dns_with_cached_fallback", "suspicious_local_dns_without_fallback")) {
        ++$suspiciousNodes
    }
    if ($nodeReport.status -in @("resolution_failed", "invalid", "suspicious_local_dns_without_fallback")) {
        ++$failedNodes
    }
}

$overallStatus = "ready"
if ($DryRun) {
    $overallStatus = "dry_run"
} elseif ($usableNodes -eq 0) {
    $overallStatus = "blocked_no_usable_bootstrap"
} elseif ($suspiciousNodes -gt 0 -or $failedNodes -gt 0) {
    $overallStatus = "warning_suspicious_or_partial_bootstrap"
}

$recommendations = @()
if ($DryRun) {
    $recommendations += "Dry run only checked parsing, known runtime fallbacks, and report generation; run without -DryRun to test local DNS and DoH."
} elseif ($overallStatus -eq "blocked_no_usable_bootstrap") {
    $recommendations += "Disable fake-IP DNS/proxy interception for the DHT bootstrap hostnames or pass numeric public --dht-bootstrap endpoints."
    $recommendations += "Run the Host/Controller DHT validation helper only after at least one bootstrap node is usable."
} elseif ($suspiciousNodes -gt 0) {
    $recommendations += "Keep the default bootstrap hostnames so the runtime can append cached numeric fallbacks, or pass numeric --dht-bootstrap endpoints for this network."
    $recommendations += "If DHT publish/fetch still fails after this preflight is usable, move the investigation to runtime DHT publish/fetch counters instead of DNS."
} else {
    $recommendations += "Bootstrap resolution looks usable; proceed to the DHT Host/Controller validation helpers or full UI run."
}

$summary = [pscustomobject]@{
    schema = "redclaw.dht.bootstrap.readiness.v1"
    generated_at = (Get-Date).ToString("o")
    mode = if ($DryRun) { "dry_run" } else { "live" }
    overall_status = $overallStatus
    requested_bootstrap = $DhtBootstrap
    runtime_note = "The libtorrent runtime appends cached numeric fallback endpoints for known default bootstrap hostnames."
    counts = [pscustomobject]@{
        requested = $nodeReports.Count
        usable_nodes = $usableNodes
        suspicious_nodes = $suspiciousNodes
        failed_nodes = $failedNodes
    }
    nodes = $nodeReports
    recommendations = $recommendations
    output_path = $OutputPath
}

Write-SummaryJson -Path $OutputPath -Summary $summary

if ($Json) {
    $summary | ConvertTo-Json -Depth 8
} else {
    Write-Host "[dht-bootstrap] status: $overallStatus"
    Write-Host "[dht-bootstrap] report: $OutputPath"
    foreach ($nodeReport in $nodeReports) {
        Write-Host ("[dht-bootstrap] {0} -> {1}" -f $nodeReport.input, $nodeReport.status)
    }
    foreach ($recommendation in $recommendations) {
        Write-Host "[dht-bootstrap] recommendation: $recommendation"
    }
}

if ($overallStatus -eq "blocked_no_usable_bootstrap") {
    exit 2
}
if ($failedNodes -gt 0 -or $suspiciousNodes -gt 0) {
    exit 1
}

exit 0
