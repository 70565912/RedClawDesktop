param(
    [Parameter(Mandatory = $true)]
    [string[]]$ReportDirectory,

    [string]$OutputPath = "",

    [switch]$Json
)

$ErrorActionPreference = "Stop"

function Resolve-ReportDirectory {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Report directory not found: $Path"
    }

    $item = Get-Item -Path $Path
    if (-not $item.PSIsContainer) {
        throw "Report path is not a directory: $Path"
    }

    return $item.FullName
}

function Get-TextIfExists {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path)) {
        return ""
    }

    return Get-Content -Path $Path -Raw
}

function Get-JsonIfExists {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path)) {
        return $null
    }

    return Get-Content -Path $Path -Raw | ConvertFrom-Json
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

function ConvertTo-KeyValueMap {
    param([string]$Line)

    $map = @{}
    if ([string]::IsNullOrWhiteSpace($Line)) {
        return $map
    }

    $matches = [regex]::Matches($Line, "([A-Za-z_][A-Za-z0-9_]*)=([^ ]+)")
    foreach ($match in $matches) {
        $map[$match.Groups[1].Value] = $match.Groups[2].Value
    }

    return $map
}

function Get-MapInt {
    param(
        [hashtable]$Map,
        [string]$Key,
        [int]$Default = 0
    )

    if (-not $Map.ContainsKey($Key)) {
        return $Default
    }

    $value = 0
    if ([int]::TryParse([string]$Map[$Key], [ref]$value)) {
        return $value
    }

    return $Default
}

function Get-MapBool {
    param(
        [hashtable]$Map,
        [string]$Key
    )

    if (-not $Map.ContainsKey($Key)) {
        return $null
    }

    $value = ([string]$Map[$Key]).ToLowerInvariant()
    if ($value -eq "true") {
        return $true
    }
    if ($value -eq "false") {
        return $false
    }

    return $null
}

function ConvertTo-Bool {
    param([object]$Value)

    if ($null -eq $Value) {
        return $false
    }
    if ($Value -is [bool]) {
        return [bool]$Value
    }

    return ([string]$Value).ToLowerInvariant() -eq "true"
}

function Get-RolesFromReport {
    param(
        [string]$Directory,
        [object]$Summary
    )

    if ($null -ne $Summary -and -not [string]::IsNullOrWhiteSpace([string]$Summary.role)) {
        return @([string]$Summary.role)
    }

    $roles = @()
    if (Test-Path (Join-Path $Directory "host.out.log")) {
        $roles += "host"
    }

    if (Test-Path (Join-Path $Directory "controller.out.log")) {
        $roles += "controller"
    }

    if ($roles.Count -eq 0) {
        $roles += "unknown"
    }

    return $roles
}

function Get-StdoutPath {
    param(
        [string]$Directory,
        [string]$Role,
        [object]$Summary
    )

    $rolePath = Join-Path $Directory ("{0}.out.log" -f $Role)
    if (Test-Path $rolePath) {
        return $rolePath
    }

    if ($null -ne $Summary -and -not [string]::IsNullOrWhiteSpace([string]$Summary.stdout)) {
        return [string]$Summary.stdout
    }

    $first = Get-ChildItem -Path $Directory -Filter "*.out.log" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $first) {
        return $first.FullName
    }

    return ""
}

function Get-StderrPath {
    param(
        [string]$Directory,
        [string]$Role,
        [object]$Summary
    )

    $rolePath = Join-Path $Directory ("{0}.err.log" -f $Role)
    if (Test-Path $rolePath) {
        return $rolePath
    }

    if ($null -ne $Summary -and -not [string]::IsNullOrWhiteSpace([string]$Summary.stderr)) {
        return [string]$Summary.stderr
    }

    $first = Get-ChildItem -Path $Directory -Filter "*.err.log" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $first) {
        return $first.FullName
    }

    return ""
}

function Get-ReportClassification {
    param(
        [string]$SummaryStatus,
        [bool]$HasStdout,
        [bool]$Connected,
        [bool]$DataChannelOpened,
        [bool]$DhtSignalPublished,
        [bool]$RemoteDescriptionApplied,
        [bool]$DhtReachable,
        [int]$PublishSuccess,
        [int]$FetchHits,
        [int]$DhtNodes,
        [int]$RemoteDhtExchanged,
        [string]$BootstrapStatus,
        [string]$StderrText
    )

    if ($SummaryStatus -eq "dry_run") {
        return "dry_run_no_runtime"
    }
    if ($SummaryStatus -eq "bootstrap_blocked" -or $BootstrapStatus -eq "blocked_no_usable_bootstrap") {
        return "bootstrap_blocked"
    }
    if (-not $HasStdout) {
        return "missing_runtime_stdout"
    }
    if ($Connected -or $DataChannelOpened) {
        return "connected"
    }
    if (-not $DhtReachable -or ($DhtNodes -eq 0 -and $PublishSuccess -eq 0 -and $FetchHits -eq 0)) {
        return "dht_not_reachable"
    }
    if (-not $DhtSignalPublished -and $PublishSuccess -eq 0) {
        return "dht_publish_missing"
    }
    if (-not $RemoteDescriptionApplied -and $FetchHits -eq 0) {
        return "dht_remote_record_missing"
    }
    if ($RemoteDescriptionApplied -and $RemoteDhtExchanged -eq 0) {
        return "dht_candidates_missing"
    }
    if ($RemoteDescriptionApplied -and $StderrText -match "Runtime ICE state failed") {
        return "ice_failed_after_dht_exchange"
    }
    if ($RemoteDescriptionApplied) {
        return "ice_pending_after_dht_exchange"
    }
    if ($SummaryStatus -eq "signaling_timeout") {
        return "signaling_timeout"
    }

    return "incomplete_or_unknown"
}

function Get-ReportWarnings {
    param(
        [bool]$Connected,
        [bool]$DhtSignalPublished,
        [int]$PublishSuccess,
        [int]$RemoteDhtExchanged,
        [string]$BootstrapStatus
    )

    $warnings = @()
    if ($BootstrapStatus -eq "warning_suspicious_or_partial_bootstrap") {
        $warnings += "bootstrap_readiness_warning"
    }
    if ($BootstrapStatus -eq "script_missing" -or $BootstrapStatus -eq "preflight_error" -or $BootstrapStatus -eq "missing_summary") {
        $warnings += "bootstrap_readiness_unavailable"
    }
    if ($Connected -and $DhtSignalPublished -and $PublishSuccess -eq 0) {
        $warnings += "connected_before_dht_publish_success_alert"
    }
    if ($Connected -and $RemoteDhtExchanged -eq 0) {
        $warnings += "connected_without_remote_dht_candidate_counter"
    }

    return $warnings
}

function Get-TriageHint {
    param([string]$Status)

    switch ($Status) {
        "dry_run" { return "Dry-run only; run a live Host/Controller workflow with the active code." }
        "dry_run_no_runtime" { return "Dry-run only; run the helper without -DryRun before using this as network evidence." }
        "bootstrap_blocked" { return "No usable DHT bootstrap was found; check DNS/proxy mode, UDP reachability, and numeric fallback bootstrap endpoints before launching runtime." }
        "missing_runtime_stdout" { return "Runtime stdout is missing; inspect helper setup, process launch, and report paths before classifying the network stage." }
        "connected" { return "This role connected; combine with the peer role and continue to UI playback evidence." }
        "success_connected" { return "Host/Controller CLI connection succeeded; continue to cross-LAN UI playback capture and final stream stats." }
        "dht_not_reachable" { return "DHT backend did not become reachable; inspect bootstrap readiness, dht_nodes, UDP listen/firewall, and local DNS/proxy interception." }
        "dht_publish_missing" { return "DHT route exists but local publish did not complete; inspect put alerts, last_put_num_success, publish retry timing, and UDP/firewall behavior." }
        "dht_remote_record_missing" { return "Local publish succeeded but no peer record was fetched; confirm both peers used the same code, overlapped in time, and the peer reached publish_success." }
        "dht_candidates_missing" { return "Offer/answer was exchanged but no DHT candidates were applied; inspect local_dht_direct_published, local_dht_full_published, remote_dht_latest_candidates, and ICE gathering." }
        "ice_failed_after_dht_exchange" { return "DHT rendezvous succeeded; triage ICE/NAT candidate pairs, host/srflx/relay availability, IPv6/port mapping, and whether TURN/helper is required." }
        "ice_pending_after_dht_exchange" { return "DHT rendezvous succeeded but ICE is still pending; extend wait time and inspect candidate publication/fetch progress before retrying UI playback." }
        "signaling_timeout" { return "Signaling timed out; inspect DHT publish/fetch counters and whether both roles were alive with the same code." }
        default { return "Inspect classification, bootstrap status, DHT counters, candidate stats, and raw final diagnostics before retrying." }
    }
}

function Convert-Report {
    param(
        [string]$Directory,
        [string]$Role,
        [object]$Summary
    )

    $stdoutPath = Get-StdoutPath -Directory $Directory -Role $Role -Summary $Summary
    $stderrPath = Get-StderrPath -Directory $Directory -Role $Role -Summary $Summary
    $stdoutText = Get-TextIfExists -Path $stdoutPath
    $stderrText = Get-TextIfExists -Path $stderrPath

    $runtimeStateLine = Get-LastMatchingLine -Text $stdoutText -Pattern ("Runtime state role={0}" -f $Role)
    $candidateStatsLine = Get-LastMatchingLine -Text $stdoutText -Pattern ("Runtime candidate stats role={0}" -f $Role)
    $dhtStatsLine = Get-LastMatchingLine -Text $stdoutText -Pattern ("Runtime DHT stats role={0}" -f $Role)
    $dhtBackendLine = Get-LastMatchingLine -Text $stdoutText -Pattern ("Runtime DHT backend diagnostics role={0}" -f $Role)
    $natLine = Get-LastMatchingLine -Text $stdoutText -Pattern ("Runtime NAT diagnostics role={0}" -f $Role)

    $runtimeState = ConvertTo-KeyValueMap -Line $runtimeStateLine
    $candidateStats = ConvertTo-KeyValueMap -Line $candidateStatsLine
    $dhtStats = ConvertTo-KeyValueMap -Line $dhtStatsLine
    $dhtBackend = ConvertTo-KeyValueMap -Line $dhtBackendLine
    $natStats = ConvertTo-KeyValueMap -Line $natLine

    $summaryStatus = ""
    if ($null -ne $Summary -and -not [string]::IsNullOrWhiteSpace([string]$Summary.status)) {
        $summaryStatus = [string]$Summary.status
    }

    $bootstrapStatus = ""
    $bootstrapOutputPath = ""
    $bootstrapUsableNodes = 0
    $bootstrapSuspiciousNodes = 0
    $bootstrapFailedNodes = 0
    $bootstrapBlocked = $false
    if ($null -ne $Summary -and $null -ne $Summary.bootstrap_readiness) {
        $bootstrapStatus = [string]$Summary.bootstrap_readiness.status
        $bootstrapOutputPath = [string]$Summary.bootstrap_readiness.output_path
        if ($null -ne $Summary.bootstrap_readiness.usable_nodes) {
            $bootstrapUsableNodes = [int]$Summary.bootstrap_readiness.usable_nodes
        }
        if ($null -ne $Summary.bootstrap_readiness.suspicious_nodes) {
            $bootstrapSuspiciousNodes = [int]$Summary.bootstrap_readiness.suspicious_nodes
        }
        if ($null -ne $Summary.bootstrap_readiness.failed_nodes) {
            $bootstrapFailedNodes = [int]$Summary.bootstrap_readiness.failed_nodes
        }
        $bootstrapBlocked = ConvertTo-Bool -Value $Summary.bootstrap_readiness.blocked
    }

    $connected = (ConvertTo-Bool -Value $Summary.connected) `
        -or ($stdoutText -match "desktop stream data channel opened") `
        -or ((Get-MapBool -Map $runtimeState -Key "connected") -eq $true)
    $dataChannelOpened = (ConvertTo-Bool -Value $Summary.data_channel_opened) `
        -or ($stdoutText -match "desktop stream data channel opened")
    $remoteDescriptionApplied = (ConvertTo-Bool -Value $Summary.remote_description_applied) `
        -or ($stdoutText -match ("Runtime remote description applied role={0}" -f $Role))
    $dhtSignalPublished = (ConvertTo-Bool -Value $Summary.dht_signal_published) `
        -or ($stdoutText -match ("Runtime DHT signal published role={0}" -f $Role))

    $dhtReachable = (Get-MapBool -Map $dhtStats -Key "reachable")
    if ($null -eq $dhtReachable) {
        $dhtReachable = Get-MapBool -Map $natStats -Key "dht_reachable"
    }
    if ($null -eq $dhtReachable) {
        $dhtReachable = $false
    }

    $publishSuccess = Get-MapInt -Map $dhtStats -Key "publish_success"
    $fetchHits = Get-MapInt -Map $dhtStats -Key "fetch_hits"
    $dhtNodes = Get-MapInt -Map $dhtBackend -Key "dht_nodes"
    $remoteDhtExchanged = Get-MapInt -Map $candidateStats -Key "remote_dht_exchanged"
    $localDhtDirectPublished = Get-MapInt -Map $candidateStats -Key "local_dht_direct_published"
    $localDhtFullPublished = Get-MapBool -Map $candidateStats -Key "local_dht_full_published"
    if ($null -eq $localDhtFullPublished) {
        $localDhtFullPublished = $false
    }
    $remoteDhtLatestCandidates = Get-MapInt -Map $candidateStats -Key "remote_dht_latest_candidates"
    $classification = Get-ReportClassification `
        -SummaryStatus $summaryStatus `
        -HasStdout (-not [string]::IsNullOrWhiteSpace($stdoutText)) `
        -Connected $connected `
        -DataChannelOpened $dataChannelOpened `
        -DhtSignalPublished $dhtSignalPublished `
        -RemoteDescriptionApplied $remoteDescriptionApplied `
        -DhtReachable ([bool]$dhtReachable) `
        -PublishSuccess $publishSuccess `
        -FetchHits $fetchHits `
        -DhtNodes $dhtNodes `
        -RemoteDhtExchanged $remoteDhtExchanged `
        -BootstrapStatus $bootstrapStatus `
        -StderrText $stderrText
    $warnings = @(Get-ReportWarnings `
        -Connected $connected `
        -DhtSignalPublished $dhtSignalPublished `
        -PublishSuccess $publishSuccess `
        -RemoteDhtExchanged $remoteDhtExchanged `
        -BootstrapStatus $bootstrapStatus)

    [pscustomobject]@{
        role = $Role
        report_directory = $Directory
        summary_status = $summaryStatus
        classification = $classification
        triage_hint = Get-TriageHint -Status $classification
        warnings = @($warnings)
        bootstrap_status = $bootstrapStatus
        bootstrap_output_path = $bootstrapOutputPath
        bootstrap_usable_nodes = $bootstrapUsableNodes
        bootstrap_suspicious_nodes = $bootstrapSuspiciousNodes
        bootstrap_failed_nodes = $bootstrapFailedNodes
        bootstrap_blocked = $bootstrapBlocked
        connected = $connected
        data_channel_opened = $dataChannelOpened
        dht_signal_published = $dhtSignalPublished
        remote_description_applied = $remoteDescriptionApplied
        dht_reachable = [bool]$dhtReachable
        publish_success = $publishSuccess
        fetch_hits = $fetchHits
        local_revision = Get-MapInt -Map $dhtStats -Key "local_revision"
        remote_revision = Get-MapInt -Map $dhtStats -Key "remote_revision"
        remote_dht_exchanged = $remoteDhtExchanged
        local_dht_direct_published = $localDhtDirectPublished
        local_dht_full_published = [bool]$localDhtFullPublished
        remote_dht_latest_candidates = $remoteDhtLatestCandidates
        local_srflx = Get-MapInt -Map $candidateStats -Key "local_srflx"
        local_relay = Get-MapInt -Map $candidateStats -Key "local_relay"
        remote_srflx = Get-MapInt -Map $candidateStats -Key "remote_srflx"
        remote_relay = Get-MapInt -Map $candidateStats -Key "remote_relay"
        dht_nodes = $dhtNodes
        dht_node_cache = Get-MapInt -Map $dhtBackend -Key "dht_node_cache"
        listen_ok = Get-MapInt -Map $dhtBackend -Key "listen_ok"
        listen_failed = Get-MapInt -Map $dhtBackend -Key "listen_failed"
        dht_errors = Get-MapInt -Map $dhtBackend -Key "dht_errors"
        put_alerts = Get-MapInt -Map $dhtBackend -Key "put_alerts"
        last_put_num_success = Get-MapInt -Map $dhtBackend -Key "last_put_num_success"
        external_ip_observed = $dhtBackend.ContainsKey("external_ip")
        nat_failure_class = if ($natStats.ContainsKey("failure_class")) { [string]$natStats["failure_class"] } else { "" }
        last_runtime_state = $runtimeStateLine
        last_candidate_stats = $candidateStatsLine
        last_dht_stats = $dhtStatsLine
        last_dht_backend_diagnostics = $dhtBackendLine
        last_nat_diagnostics = $natLine
        stdout = $stdoutPath
        stderr = $stderrPath
    }
}

function Expand-ReportDirectoryArguments {
    param([string[]]$Values)

    $expanded = @()
    foreach ($value in $Values) {
        foreach ($part in ([string]$value -split ",")) {
            if (-not [string]::IsNullOrWhiteSpace($part)) {
                $expanded += $part.Trim()
            }
        }
    }

    return $expanded
}

$resolvedDirectories = @()
foreach ($directory in (Expand-ReportDirectoryArguments -Values $ReportDirectory)) {
    $resolvedDirectories += Resolve-ReportDirectory -Path $directory
}

$reports = @()
foreach ($directory in $resolvedDirectories) {
    $summaryPath = Join-Path $directory "summary.json"
    $summary = Get-JsonIfExists -Path $summaryPath
    foreach ($role in (Get-RolesFromReport -Directory $directory -Summary $summary)) {
        $reports += Convert-Report -Directory $directory -Role $role -Summary $summary
    }
}

$overallStatus = "unknown"
$connectedCount = @($reports | Where-Object { $_.classification -eq "connected" }).Count
$dryRunCount = @($reports | Where-Object { $_.classification -eq "dry_run_no_runtime" }).Count
$bootstrapBlockedCount = @($reports | Where-Object { $_.classification -eq "bootstrap_blocked" }).Count
$dhtNotReachableCount = @($reports | Where-Object { $_.classification -eq "dht_not_reachable" }).Count
$dhtPublishMissingCount = @($reports | Where-Object { $_.classification -eq "dht_publish_missing" }).Count
$dhtRemoteRecordMissingCount = @($reports | Where-Object { $_.classification -eq "dht_remote_record_missing" }).Count
$dhtCandidatesMissingCount = @($reports | Where-Object { $_.classification -eq "dht_candidates_missing" }).Count
$iceFailedCount = @($reports | Where-Object { $_.classification -eq "ice_failed_after_dht_exchange" }).Count
$icePendingCount = @($reports | Where-Object { $_.classification -eq "ice_pending_after_dht_exchange" }).Count
$missingStdoutCount = @($reports | Where-Object { $_.classification -eq "missing_runtime_stdout" }).Count

if ($reports.Count -gt 0 -and $connectedCount -eq $reports.Count) {
    $overallStatus = "success_connected"
} elseif ($dryRunCount -gt 0) {
    $overallStatus = "dry_run"
} elseif ($bootstrapBlockedCount -gt 0) {
    $overallStatus = "bootstrap_blocked"
} elseif ($dhtNotReachableCount -gt 0) {
    $overallStatus = "dht_not_reachable"
} elseif ($dhtPublishMissingCount -gt 0) {
    $overallStatus = "dht_publish_missing"
} elseif ($dhtRemoteRecordMissingCount -gt 0) {
    $overallStatus = "dht_remote_record_missing"
} elseif ($dhtCandidatesMissingCount -gt 0) {
    $overallStatus = "dht_candidates_missing"
} elseif ($iceFailedCount -gt 0) {
    $overallStatus = "ice_failed_after_dht_exchange"
} elseif ($icePendingCount -gt 0) {
    $overallStatus = "ice_pending_after_dht_exchange"
} elseif ($missingStdoutCount -gt 0) {
    $overallStatus = "missing_runtime_stdout"
} else {
    $overallStatus = "incomplete_or_unknown"
}

$result = [pscustomobject]@{
    overall_status = $overallStatus
    triage_hint = Get-TriageHint -Status $overallStatus
    report_count = $reports.Count
    generated_at = (Get-Date).ToString("o")
    reports = $reports
}

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $parent = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $result | ConvertTo-Json -Depth 6 | Set-Content -Path $OutputPath -Encoding UTF8
}

if ($Json) {
    $result | ConvertTo-Json -Depth 6
} else {
    Write-Host ("[dht-summary] overall_status: {0}" -f $overallStatus)
    foreach ($report in $reports) {
        $warnings = @($report.warnings)
        $warningText = ""
        if ($warnings.Count -gt 0) {
            $warningText = " warnings=" + ($warnings -join ",")
        }
        $bootstrapText = ""
        if (-not [string]::IsNullOrWhiteSpace($report.bootstrap_status)) {
            $bootstrapText = " bootstrap_status=" + $report.bootstrap_status
        }
        Write-Host (
            "[dht-summary] role={0} classification={1} connected={2} dht_reachable={3} publish_success={4} fetch_hits={5} remote_dht_exchanged={6} dht_nodes={7} local_direct={8} local_full={9} remote_latest={10}{11}{12}" -f `
                $report.role,
                $report.classification,
                $report.connected,
                $report.dht_reachable,
                $report.publish_success,
                $report.fetch_hits,
                $report.remote_dht_exchanged,
                $report.dht_nodes,
                $report.local_dht_direct_published,
                $report.local_dht_full_published,
                $report.remote_dht_latest_candidates,
                $bootstrapText,
                $warningText
        )
        Write-Host ("[dht-summary] hint role={0}: {1}" -f $report.role, $report.triage_hint)
    }

    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        Write-Host ("[dht-summary] wrote: {0}" -f $OutputPath)
    }
}

if ($overallStatus -eq "success_connected" -or $overallStatus -eq "dry_run") {
    exit 0
}

exit 1
