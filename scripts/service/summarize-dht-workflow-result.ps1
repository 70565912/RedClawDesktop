param(
    [Parameter(Mandatory = $true)]
    [string[]]$WorkflowResult,

    [string[]]$EvidenceZip = @(),

    [string]$OutputPath = "",

    [switch]$Json
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $candidate = Resolve-Path (Join-Path $PSScriptRoot "..\..")
    return $candidate.Path
}

function Expand-ArgumentList {
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

function Resolve-ExistingFile {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Path is required."
    }
    if (-not (Test-Path $Path)) {
        throw "File not found: $Path"
    }

    $item = Get-Item -Path $Path
    if ($item.PSIsContainer) {
        throw "Expected file but found directory: $Path"
    }

    return $item.FullName
}

function Resolve-OptionalPath {
    param(
        [string]$Path,
        [string]$BaseDirectory = ""
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }
    if (Test-Path $Path) {
        return (Resolve-Path $Path).Path
    }
    if (-not [string]::IsNullOrWhiteSpace($BaseDirectory)) {
        $candidate = Join-Path $BaseDirectory $Path
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    return ""
}

function Get-JsonFile {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path)) {
        return $null
    }

    return Get-Content -Path $Path -Raw | ConvertFrom-Json
}

function Get-ZipJsonEntry {
    param(
        [string]$ZipPath,
        [string]$EntryName
    )

    if ([string]::IsNullOrWhiteSpace($ZipPath) -or -not (Test-Path $ZipPath)) {
        return $null
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $normalizedEntryName = $EntryName.Replace("\", "/")
        $entry = $zip.Entries |
            Where-Object {
                $name = $_.FullName.Replace("\", "/")
                $name -eq $normalizedEntryName -or $name.EndsWith("/$normalizedEntryName")
            } |
            Select-Object -First 1

        if ($null -eq $entry) {
            return $null
        }

        $stream = $entry.Open()
        try {
            $reader = [System.IO.StreamReader]::new($stream)
            try {
                return ($reader.ReadToEnd() | ConvertFrom-Json)
            } finally {
                $reader.Dispose()
            }
        } finally {
            $stream.Dispose()
        }
    } finally {
        $zip.Dispose()
    }
}

function Find-EvidenceZip {
    param(
        [object]$Workflow,
        [string[]]$ProvidedZips,
        [string]$WorkflowResultDirectory
    )

    $expectedZip = Resolve-OptionalPath -Path ([string]$Workflow.archive_zip)
    if (-not [string]::IsNullOrWhiteSpace($expectedZip)) {
        return $expectedZip
    }

    $expectedLeaf = ""
    if (-not [string]::IsNullOrWhiteSpace([string]$Workflow.archive_zip)) {
        $expectedLeaf = Split-Path -Leaf ([string]$Workflow.archive_zip)
    }

    foreach ($zip in $ProvidedZips) {
        $resolvedZip = Resolve-OptionalPath -Path $zip -BaseDirectory $WorkflowResultDirectory
        if ([string]::IsNullOrWhiteSpace($resolvedZip)) {
            continue
        }
        if ([string]::IsNullOrWhiteSpace($expectedLeaf) -or (Split-Path -Leaf $resolvedZip) -eq $expectedLeaf) {
            return $resolvedZip
        }
    }

    return ""
}

function Get-EvidenceZipStatus {
    param(
        [object]$Workflow,
        [string]$ZipPath
    )

    if ([string]::IsNullOrWhiteSpace([string]$Workflow.archive_zip)) {
        return "not_requested"
    }
    if ([string]::IsNullOrWhiteSpace($ZipPath)) {
        return "missing"
    }
    if ([string]::IsNullOrWhiteSpace([string]$Workflow.archive_zip_sha256)) {
        return "present_no_expected_hash"
    }

    $actual = (Get-FileHash -Algorithm SHA256 -Path $ZipPath).Hash.ToLowerInvariant()
    if ($actual -eq ([string]$Workflow.archive_zip_sha256).ToLowerInvariant()) {
        return "present_hash_ok"
    }

    return "present_hash_mismatch"
}

function Get-WorkflowClassification {
    param(
        [object]$Workflow,
        [object]$Summary
    )

    $workflowStatus = [string]$Workflow.status
    if ($workflowStatus -ne "success") {
        if ([string]$Workflow.error_message -match "Runtime executable not found") {
            return "runtime_missing"
        }
        if ($workflowStatus -eq "helper_failed" -and $null -ne $Summary -and -not [string]::IsNullOrWhiteSpace([string]$Summary.overall_status)) {
            return [string]$Summary.overall_status
        }
        return $workflowStatus
    }

    if ($null -eq $Summary) {
        return "summary_missing"
    }

    return [string]$Summary.overall_status
}

function Get-OverallStatus {
    param([object[]]$Items)

    $classifications = @($Items | ForEach-Object { [string]$_.classification })
    foreach ($status in @(
            "setup_failed",
            "runtime_missing",
            "helper_failed",
            "report_missing",
            "summary_failed",
            "archive_failed",
            "summary_missing",
            "bootstrap_blocked",
            "dht_not_reachable",
            "dht_publish_missing",
            "dht_remote_record_missing",
            "dht_candidates_missing",
            "ice_failed_after_dht_exchange",
            "ice_pending_after_dht_exchange",
            "missing_runtime_stdout",
            "dry_run",
            "incomplete_or_unknown")) {
        if ($classifications -contains $status) {
            return $status
        }
    }

    if ($classifications.Count -gt 0 -and @($classifications | Where-Object { $_ -eq "success_connected" }).Count -eq $classifications.Count) {
        return "success_connected"
    }

    return "incomplete_or_unknown"
}

function Get-RoleCoverage {
    param([object[]]$Items)

    $roles = @()
    foreach ($item in @($Items)) {
        $role = [string]$item.role
        if ($role -eq "host" -or $role -eq "controller") {
            $roles += $role
        }

        foreach ($report in @($item.report_classifications)) {
            $reportRole = [string]$report.role
            if ($reportRole -eq "host" -or $reportRole -eq "controller") {
                $roles += $reportRole
            }
        }
    }

    $uniqueRoles = @($roles | Sort-Object -Unique)
    $hasHost = $uniqueRoles -contains "host"
    $hasController = $uniqueRoles -contains "controller"
    $missingRoles = @()
    if (-not $hasHost) {
        $missingRoles += "host"
    }
    if (-not $hasController) {
        $missingRoles += "controller"
    }

    $scope = "unknown"
    if ($hasHost -and $hasController) {
        $scope = "host_controller_cli_pair"
    } elseif ($uniqueRoles.Count -eq 1) {
        $scope = "single_role_cli"
    }

    return [pscustomobject]@{
        workflow_scope = $scope
        roles = @($uniqueRoles)
        has_host = $hasHost
        has_controller = $hasController
        missing_roles = @($missingRoles)
    }
}

function Get-P0Ds07GateStatus {
    param(
        [string]$OverallStatus,
        [object]$RoleCoverage
    )

    if ($OverallStatus -eq "dry_run") {
        return "not_evidence_dry_run"
    }
    if ($OverallStatus -ne "success_connected") {
        return "triage_required_before_ui_playback"
    }
    if ([bool]$RoleCoverage.has_host -and [bool]$RoleCoverage.has_controller) {
        return "cli_pair_connected_ui_playback_required"
    }

    return "single_role_cli_connected_peer_result_required"
}

function Get-NextAction {
    param([string]$GateStatus)

    switch ($GateStatus) {
        "cli_pair_connected_ui_playback_required" {
            return "Proceed to Controller UI playback capture; CLI workflow success alone is not the P0-DS-07 proof."
        }
        "single_role_cli_connected_peer_result_required" {
            return "Collect the peer role workflow result, then run UI playback if the combined CLI summary stays success_connected."
        }
        "not_evidence_dry_run" {
            return "Run a live Host/Controller workflow with the active remote code."
        }
        default {
            return "Triage the reported classification, bootstrap status, DHT counters, and ICE candidate stats before retrying UI playback."
        }
    }
}

function Get-TriageHint {
    param([string]$Status)

    switch ($Status) {
        "dry_run" { return "Dry-run only; run a live Host/Controller workflow with the active code." }
        "runtime_missing" { return "Runtime executable is missing; run prepare-dht-remote-validation and rebuild/publish before network testing." }
        "setup_failed" { return "Workflow setup failed; inspect error_message and required script/runtime paths before launching DHT runtime." }
        "helper_failed" { return "Helper exited non-zero without a more precise embedded summary; inspect helper stdout/stderr and error_message." }
        "report_missing" { return "Workflow report directory is missing; rerun the role workflow and preserve the generated result JSON." }
        "summary_failed" { return "Report summarization failed; inspect summary_exit_code and rerun the summarizer on the report directory." }
        "archive_failed" { return "Evidence archive failed; inspect archive_exit_code but keep the report summary for network triage." }
        "summary_missing" { return "No workflow summary or archived dht-summary was found; provide result-summary JSON or evidence zip for classification." }
        "bootstrap_blocked" { return "No usable DHT bootstrap was found; check DNS/proxy mode, UDP reachability, and numeric fallback bootstrap endpoints before launching runtime." }
        "missing_runtime_stdout" { return "Runtime stdout is missing; inspect helper setup, process launch, and report paths before classifying the network stage." }
        "success_connected" { return "Host/Controller CLI connection succeeded; continue to cross-LAN UI playback capture and final stream stats." }
        "connected" { return "This report connected; combine both roles and continue to UI playback evidence." }
        "dht_not_reachable" { return "DHT backend did not become reachable; inspect bootstrap readiness, dht_nodes, UDP listen/firewall, and local DNS/proxy interception." }
        "dht_publish_missing" { return "DHT route exists but local publish did not complete; inspect put alerts, last_put_num_success, publish retry timing, and UDP/firewall behavior." }
        "dht_remote_record_missing" { return "Local publish succeeded but no peer record was fetched; confirm both peers used the same code, overlapped in time, and the peer reached publish_success." }
        "dht_candidates_missing" { return "Offer/answer was exchanged but no DHT candidates were applied; inspect local_dht_direct_published, local_dht_full_published, remote_dht_latest_candidates, and ICE gathering." }
        "ice_failed_after_dht_exchange" { return "DHT rendezvous succeeded; triage ICE/NAT candidate pairs, host/srflx/relay availability, IPv6/port mapping, and whether TURN/helper is required." }
        "ice_pending_after_dht_exchange" { return "DHT rendezvous succeeded but ICE is still pending; extend wait time and inspect candidate publication/fetch progress before retrying UI playback." }
        default { return "Inspect classification, bootstrap status, DHT counters, candidate stats, and raw final diagnostics before retrying." }
    }
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$resolvedWorkflowResults = @()
foreach ($path in (Expand-ArgumentList -Values $WorkflowResult)) {
    $resolvedWorkflowResults += Resolve-ExistingFile -Path $path
}
if ($resolvedWorkflowResults.Count -eq 0) {
    throw "At least one workflow result JSON is required."
}

$providedZips = @(Expand-ArgumentList -Values $EvidenceZip)
$items = @()
foreach ($workflowResultPath in $resolvedWorkflowResults) {
    $workflowResultDirectory = Split-Path -Parent $workflowResultPath
    $workflow = Get-JsonFile -Path $workflowResultPath
    if ($null -eq $workflow) {
        throw "Failed to parse workflow result: $workflowResultPath"
    }

    $summaryPath = Resolve-OptionalPath -Path ([string]$workflow.summary) -BaseDirectory $workflowResultDirectory
    $archivePath = Resolve-OptionalPath -Path ([string]$workflow.archive) -BaseDirectory $workflowResultDirectory
    $zipPath = Find-EvidenceZip -Workflow $workflow -ProvidedZips $providedZips -WorkflowResultDirectory $workflowResultDirectory
    $summarySource = "missing"
    $summary = $null

    if (-not [string]::IsNullOrWhiteSpace($summaryPath)) {
        $summary = Get-JsonFile -Path $summaryPath
        $summarySource = "workflow_summary"
    }
    if ($null -eq $summary -and -not [string]::IsNullOrWhiteSpace($archivePath)) {
        $archiveSummaryPath = Join-Path $archivePath "dht-summary.json"
        $summary = Get-JsonFile -Path $archiveSummaryPath
        if ($null -ne $summary) {
            $summarySource = "archive_directory"
        }
    }
    if ($null -eq $summary -and -not [string]::IsNullOrWhiteSpace($zipPath)) {
        $summary = Get-ZipJsonEntry -ZipPath $zipPath -EntryName "dht-summary.json"
        if ($null -ne $summary) {
            $summarySource = "archive_zip"
        }
    }

    $toolchainSummary = $null
    if (-not [string]::IsNullOrWhiteSpace($archivePath)) {
        $toolchainSummary = Get-JsonFile -Path (Join-Path $archivePath "toolchain-selftest-summary.json")
    }
    if ($null -eq $toolchainSummary -and -not [string]::IsNullOrWhiteSpace($zipPath)) {
        $toolchainSummary = Get-ZipJsonEntry -ZipPath $zipPath -EntryName "toolchain-selftest-summary.json"
    }

    $zipStatus = Get-EvidenceZipStatus -Workflow $workflow -ZipPath $zipPath
    $classification = Get-WorkflowClassification -Workflow $workflow -Summary $summary
    $reportClassifications = @()
    if ($null -ne $summary -and $null -ne $summary.reports) {
        foreach ($report in @($summary.reports)) {
            $reportClassification = [string]$report.classification
            $reportTriageHint = [string]$report.triage_hint
            if ([string]::IsNullOrWhiteSpace($reportTriageHint)) {
                $reportTriageHint = Get-TriageHint -Status $reportClassification
            }
            $reportClassifications += [pscustomobject]@{
                role = [string]$report.role
                classification = $reportClassification
                triage_hint = $reportTriageHint
                bootstrap_status = [string]$report.bootstrap_status
                connected = [bool]$report.connected
                dht_reachable = [bool]$report.dht_reachable
                publish_success = [int]$report.publish_success
                fetch_hits = [int]$report.fetch_hits
                remote_dht_exchanged = [int]$report.remote_dht_exchanged
                local_dht_direct_published = [int]$report.local_dht_direct_published
                local_dht_full_published = [bool]$report.local_dht_full_published
                remote_dht_latest_candidates = [int]$report.remote_dht_latest_candidates
                dht_nodes = [int]$report.dht_nodes
                nat_failure_class = [string]$report.nat_failure_class
                warnings = @($report.warnings)
                last_candidate_stats = [string]$report.last_candidate_stats
                last_dht_stats = [string]$report.last_dht_stats
                last_dht_backend_diagnostics = [string]$report.last_dht_backend_diagnostics
                last_nat_diagnostics = [string]$report.last_nat_diagnostics
            }
        }
    }

    $items += [pscustomobject]@{
        workflow_result = $workflowResultPath
        status = [string]$workflow.status
        classification = $classification
        triage_hint = Get-TriageHint -Status $classification
        role = [string]$workflow.role
        mode = [string]$workflow.mode
        session_code_redacted = [string]$workflow.session_code_redacted
        report_directory = if ([string]::IsNullOrWhiteSpace([string]$workflow.report_directory)) { $null } else { [string]$workflow.report_directory }
        summary = if ([string]::IsNullOrWhiteSpace([string]$workflow.summary)) { $null } else { [string]$workflow.summary }
        summary_source = $summarySource
        summary_overall_status = if ($null -eq $summary) { $null } else { [string]$summary.overall_status }
        report_count = if ($null -eq $summary -or $null -eq $summary.report_count) { 0 } else { [int]$summary.report_count }
        report_classifications = @($reportClassifications)
        archive = if ([string]::IsNullOrWhiteSpace([string]$workflow.archive)) { $null } else { [string]$workflow.archive }
        archive_zip = if ([string]::IsNullOrWhiteSpace([string]$workflow.archive_zip)) { $null } else { [string]$workflow.archive_zip }
        archive_zip_resolved = if ([string]::IsNullOrWhiteSpace($zipPath)) { $null } else { $zipPath }
        archive_zip_status = $zipStatus
        archive_zip_sha256 = [string]$workflow.archive_zip_sha256
        helper_exit_code = $workflow.helper_exit_code
        summary_exit_code = $workflow.summary_exit_code
        archive_exit_code = $workflow.archive_exit_code
        error_message = if ([string]::IsNullOrWhiteSpace([string]$workflow.error_message)) { $null } else { [string]$workflow.error_message }
        toolchain_self_test_status = if ($null -eq $toolchainSummary) { $null } else { [string]$toolchainSummary.overall_status }
        toolchain_self_test_included = [bool]$workflow.toolchain_self_test_included
        existing_report_regression_included = [bool]$workflow.existing_report_regression_included
    }
}

$overallStatus = Get-OverallStatus -Items $items
$roleCoverage = Get-RoleCoverage -Items $items
$p0Ds07GateStatus = Get-P0Ds07GateStatus -OverallStatus $overallStatus -RoleCoverage $roleCoverage

$result = [pscustomobject]@{
    schema = "redclaw.dht.workflow.result.summary.v1"
    generated_at = (Get-Date).ToString("o")
    overall_status = $overallStatus
    triage_hint = Get-TriageHint -Status $overallStatus
    role_coverage = $roleCoverage
    p0_ds07_gate_status = $p0Ds07GateStatus
    next_action = Get-NextAction -GateStatus $p0Ds07GateStatus
    workflow_result_count = $items.Count
    workflows = @($items)
}

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $parent = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $result | ConvertTo-Json -Depth 8 | Set-Content -Path $OutputPath -Encoding UTF8
}

if ($Json) {
    $result | ConvertTo-Json -Depth 8
} else {
    Write-Host ("[dht-workflow-summary] overall_status: {0}" -f $result.overall_status)
    Write-Host ("[dht-workflow-summary] triage_hint: {0}" -f $result.triage_hint)
    Write-Host ("[dht-workflow-summary] role_scope: {0} roles={1} missing_roles={2}" -f `
            $result.role_coverage.workflow_scope,
            ($result.role_coverage.roles -join ","),
            ($result.role_coverage.missing_roles -join ","))
    Write-Host ("[dht-workflow-summary] p0_ds07_gate_status: {0}" -f $result.p0_ds07_gate_status)
    Write-Host ("[dht-workflow-summary] next_action: {0}" -f $result.next_action)
    foreach ($workflow in @($result.workflows)) {
        Write-Host (
            "[dht-workflow-summary] role={0} mode={1} status={2} classification={3} summary={4} zip_status={5} toolchain={6}" -f `
                $workflow.role,
                $workflow.mode,
                $workflow.status,
                $workflow.classification,
                $workflow.summary_overall_status,
                $workflow.archive_zip_status,
                $workflow.toolchain_self_test_status)
        Write-Host ("[dht-workflow-summary] hint role={0}: {1}" -f $workflow.role, $workflow.triage_hint)
        if (-not [string]::IsNullOrWhiteSpace($workflow.error_message)) {
            Write-Host ("[dht-workflow-summary] error: {0}" -f $workflow.error_message)
        }
        foreach ($report in @($workflow.report_classifications)) {
            $warnings = @($report.warnings)
            $warningText = ""
            if ($warnings.Count -gt 0) {
                $warningText = " warnings=" + ($warnings -join ",")
            }
            Write-Host (
                "[dht-workflow-summary] report role={0} classification={1} connected={2} dht_reachable={3} publish_success={4} fetch_hits={5} remote_dht_exchanged={6} dht_nodes={7} local_direct={8} local_full={9} remote_latest={10} bootstrap_status={11}{12}" -f `
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
                    $report.bootstrap_status,
                    $warningText)
            Write-Host ("[dht-workflow-summary] report_hint role={0}: {1}" -f $report.role, $report.triage_hint)
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        Write-Host ("[dht-workflow-summary] wrote: {0}" -f $OutputPath)
    }
}

$exitCode = if ($result.overall_status -eq "success_connected" -or $result.overall_status -eq "dry_run") { 0 } else { 1 }
exit $exitCode
