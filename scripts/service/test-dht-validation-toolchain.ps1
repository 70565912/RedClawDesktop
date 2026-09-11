param(
    [string]$ReportRoot = "build\reports",

    [string]$OutputPath = "",

    [switch]$IncludeExistingReportRegression,

    [switch]$Json
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $candidate = Resolve-Path (Join-Path $PSScriptRoot "..\..")
    return $candidate.Path
}

function Write-JsonFile {
    param(
        [string]$Path,
        [object]$Value
    )

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    $Value | ConvertTo-Json -Depth 8 | Set-Content -Path $Path -Encoding UTF8
}

function New-CaseDirectory {
    param(
        [string]$Root,
        [string]$Name
    )

    $path = Join-Path $Root $Name
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    return $path
}

function New-ConnectedRoleReport {
    param(
        [string]$Directory,
        [string]$Role,
        [int]$DhtNodes
    )

    $stdoutPath = Join-Path $Directory ("{0}.out.log" -f $Role)
    $stderrPath = Join-Path $Directory ("{0}.err.log" -f $Role)
    $bootstrapPath = Join-Path $Directory "bootstrap-readiness.json"

    $stdout = @(
        ("Runtime remote description applied role={0} (dht)" -f $Role),
        ("Runtime DHT signal published role={0} revision=2 candidates=1 phase=priority-candidates" -f $Role),
        ("Runtime state role={0} state=3 remote_description_applied=true connected=true" -f $Role),
        ("Runtime candidate stats role={0} local_total=5 local_host=4 local_srflx=1 local_relay=0 local_prflx=0 local_unknown=0 local_ipv6=0 remote_total=1 remote_host=0 remote_srflx=1 remote_relay=0 remote_prflx=0 remote_unknown=0 remote_ipv6=0 remote_dht_exchanged=1 local_dht_direct_published=3 local_dht_full_published=true remote_dht_latest_candidates=5" -f $Role),
        ("Runtime DHT stats role={0} backend=libtorrent-mainline+file-mailbox reachable=true publish_attempts=4 publish_success=2 fetch_attempts=4 fetch_hits=2 local_revision=2 remote_revision=2" -f $Role),
        ("Runtime DHT backend diagnostics role={0} dht_nodes={1} dht_node_cache=12 listen_ok=2 listen_failed=0 dht_errors=0 put_alerts=2 last_put_num_success=10 external_ip=203.0.113.10" -f $Role, $DhtNodes),
        ("Runtime NAT diagnostics role={0} failure_class=no_ipv6 dht_reachable=true" -f $Role),
        $(if ($Role -eq "host") {
            "Runtime desktop stream stats role=host channel_open=true capture_backend=GdiBitBlt captured=12 synthetic=0 capture_failures=0 encoded=10 encode_failures=0 transmitted=10 transmit_failures=0 received=0 decoded=0 decode_failures=0 rendered=0 render_failures=0"
        } else {
            "Runtime desktop stream stats role=controller channel_open=true capture_backend=none captured=0 synthetic=0 capture_failures=0 encoded=0 encode_failures=0 transmitted=0 transmit_failures=0 received=10 media_fragments_received=20 encoded_frames_reassembled=10 direct_pipe_written=10 decoded=0 decode_failures=0 rendered=0 render_failures=0"
        }),
        $(if ($Role -eq "controller") {
            "GUI direct frame stats writer_fps=2 publish_fps=2 playback_fps=2 decode_failures=0 present_failures=0 presented_total=10 gui_decode_success_total=10 health=healthy"
        }),
        "desktop stream data channel opened"
    )
    $stdout | Set-Content -Path $stdoutPath -Encoding UTF8
    "" | Set-Content -Path $stderrPath -Encoding UTF8

    $bootstrap = [pscustomobject]@{
        schema = "redclaw.dht.bootstrap.readiness.v1"
        generated_at = (Get-Date).ToString("o")
        mode = "synthetic"
        overall_status = "warning_suspicious_or_partial_bootstrap"
        requested_bootstrap = @("router.bittorrent.com:6881")
        counts = [pscustomobject]@{
            requested = 1
            usable_nodes = 1
            suspicious_nodes = 1
            failed_nodes = 0
        }
        nodes = @()
        recommendations = @("synthetic self-test fixture")
        output_path = $bootstrapPath
    }
    Write-JsonFile -Path $bootstrapPath -Value $bootstrap

    $summary = [pscustomobject]@{
        status = "success_connected"
        role = $Role
        session_code_redacted = "SELF****"
        runtime_exe = "synthetic"
        report_directory = $Directory
        stdout = $stdoutPath
        stderr = $stderrPath
        arguments_redacted = @("--session-code", "SELF****")
        bootstrap_readiness = [pscustomobject]@{
            enabled = $true
            status = "warning_suspicious_or_partial_bootstrap"
            output_path = $bootstrapPath
            exit_code = 1
            usable_nodes = 1
            suspicious_nodes = 1
            failed_nodes = 0
            blocked = $false
            error = ""
        }
        started_at = (Get-Date).ToString("o")
        finished_at = (Get-Date).ToString("o")
        connected = $true
        data_channel_opened = $true
        dht_signal_published = $true
        remote_description_applied = $true
    }
    Write-JsonFile -Path (Join-Path $Directory "summary.json") -Value $summary
}

function New-DhtFailureRoleReport {
    param(
        [string]$Directory,
        [string]$Role,
        [string]$Case,
        [bool]$DhtReachable,
        [int]$PublishSuccess,
        [int]$FetchHits,
        [int]$DhtNodes,
        [int]$RemoteDhtExchanged,
        [bool]$DhtSignalPublished,
        [bool]$RemoteDescriptionApplied,
        [string]$StderrLine = ""
    )

    $stdoutPath = Join-Path $Directory ("{0}.out.log" -f $Role)
    $stderrPath = Join-Path $Directory ("{0}.err.log" -f $Role)

    $stdout = @()
    if ($RemoteDescriptionApplied) {
        $stdout += ("Runtime remote description applied role={0} (dht)" -f $Role)
    }
    if ($DhtSignalPublished) {
        $stdout += ("Runtime DHT signal published role={0} revision=1 candidates=0 phase=description-only" -f $Role)
    }
    $stdout += ("Runtime state role={0} state=2 remote_description_applied={1} connected=false" -f $Role, $RemoteDescriptionApplied.ToString().ToLowerInvariant())
    $stdout += ("Runtime candidate stats role={0} local_total=3 local_host=2 local_srflx=1 local_relay=0 local_prflx=0 local_unknown=0 local_ipv6=0 remote_total={1} remote_host=0 remote_srflx={2} remote_relay=0 remote_prflx=0 remote_unknown=0 remote_ipv6=0 remote_dht_exchanged={3} local_dht_direct_published=3 local_dht_full_published={4} remote_dht_latest_candidates={5}" -f $Role, $RemoteDhtExchanged, $RemoteDhtExchanged, $RemoteDhtExchanged, $(if ($RemoteDhtExchanged -gt 1) { "true" } else { "false" }), $RemoteDhtExchanged)
    $stdout += ("Runtime DHT stats role={0} backend=libtorrent-mainline+file-mailbox reachable={1} publish_attempts=4 publish_success={2} fetch_attempts=4 fetch_hits={3} local_revision={2} remote_revision={3} last_error={4}_synthetic_failure" -f $Role, $DhtReachable.ToString().ToLowerInvariant(), $PublishSuccess, $FetchHits, $Case)
    $stdout += ("Runtime DHT backend diagnostics role={0} dht_nodes={1} dht_node_cache=12 listen_ok=2 listen_failed=0 dht_errors=0 put_alerts={2} last_put_num_success={3} external_ip=203.0.113.10" -f $Role, $DhtNodes, $PublishSuccess, $(if ($PublishSuccess -gt 0) { 8 } else { 0 }))
    $stdout += ("Runtime NAT diagnostics role={0} failure_class=no_ipv6 dht_reachable={1}" -f $Role, $DhtReachable.ToString().ToLowerInvariant())
    $stdout | Set-Content -Path $stdoutPath -Encoding UTF8
    $StderrLine | Set-Content -Path $stderrPath -Encoding UTF8

    $summary = [pscustomobject]@{
        status = "timeout_before_success"
        role = $Role
        session_code_redacted = "SELF****"
        runtime_exe = "synthetic"
        report_directory = $Directory
        stdout = $stdoutPath
        stderr = $stderrPath
        arguments_redacted = @("--session-code", "SELF****")
        bootstrap_readiness = [pscustomobject]@{
            enabled = $true
            status = "ready"
            output_path = ""
            exit_code = 0
            usable_nodes = 1
            suspicious_nodes = 0
            failed_nodes = 0
            blocked = $false
            error = ""
        }
        started_at = (Get-Date).ToString("o")
        finished_at = (Get-Date).ToString("o")
        connected = $false
        data_channel_opened = $false
        dht_signal_published = $DhtSignalPublished
        remote_description_applied = $RemoteDescriptionApplied
    }
    Write-JsonFile -Path (Join-Path $Directory "summary.json") -Value $summary
}

function Invoke-Summarizer {
    param(
        [string]$SummarizerScript,
        [string[]]$ReportDirectory,
        [string]$OutputPath,
        [int]$ExpectedExitCode
    )

    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $SummarizerScript,
        "-ReportDirectory"
    ) + @(
        ($ReportDirectory -join ",")
    ) + @(
        "-OutputPath",
        $OutputPath,
        "-Json"
    )

    $output = & powershell.exe @arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne $ExpectedExitCode) {
        throw "Summarizer exit code $exitCode did not match expected $ExpectedExitCode. Output: $($output -join ' ')"
    }
    if (-not (Test-Path $OutputPath)) {
        throw "Summarizer did not write expected output: $OutputPath"
    }

    return (Get-Content -Path $OutputPath -Raw | ConvertFrom-Json)
}

function Invoke-WorkflowSummarizer {
    param(
        [string]$WorkflowSummarizerScript,
        [string[]]$WorkflowResult,
        [string]$OutputPath,
        [int]$ExpectedExitCode
    )

    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $WorkflowSummarizerScript,
        "-WorkflowResult"
    ) + @(
        ($WorkflowResult -join ",")
    ) + @(
        "-OutputPath",
        $OutputPath,
        "-Json"
    )

    $output = & powershell.exe @arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne $ExpectedExitCode) {
        throw "Workflow summarizer exit code $exitCode did not match expected $ExpectedExitCode. Output: $($output -join ' ')"
    }
    if (-not (Test-Path $OutputPath)) {
        throw "Workflow summarizer did not write expected output: $OutputPath"
    }

    return (Get-Content -Path $OutputPath -Raw | ConvertFrom-Json)
}

function Invoke-WorkflowSummarizerConsole {
    param(
        [string]$WorkflowSummarizerScript,
        [string[]]$WorkflowResult,
        [string]$OutputPath,
        [int]$ExpectedExitCode
    )

    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $WorkflowSummarizerScript,
        "-WorkflowResult"
    ) + @(
        ($WorkflowResult -join ",")
    ) + @(
        "-OutputPath",
        $OutputPath
    )

    $output = & powershell.exe @arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne $ExpectedExitCode) {
        throw "Workflow summarizer console exit code $exitCode did not match expected $ExpectedExitCode. Output: $($output -join ' ')"
    }
    if (-not (Test-Path $OutputPath)) {
        throw "Workflow summarizer console run did not write expected output: $OutputPath"
    }

    return ($output -join "`n")
}

function Invoke-P0Gate {
    param(
        [string]$P0GateScript,
        [string]$WorkflowSummary,
        [string[]]$UiEvidence,
        [string[]]$HostRuntimeLog,
        [string[]]$ControllerRuntimeLog,
        [string]$OutputPath,
        [int]$ExpectedExitCode,
        [switch]$CrossLanConfirmed
    )

    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $P0GateScript,
        "-WorkflowSummary",
        $WorkflowSummary,
        "-OutputPath",
        $OutputPath,
        "-Json"
    )
    if ($UiEvidence.Count -gt 0) {
        $arguments += "-UiEvidence"
        $arguments += ($UiEvidence -join ",")
    }
    if ($HostRuntimeLog.Count -gt 0) {
        $arguments += "-HostRuntimeLog"
        $arguments += ($HostRuntimeLog -join ",")
    }
    if ($ControllerRuntimeLog.Count -gt 0) {
        $arguments += "-ControllerRuntimeLog"
        $arguments += ($ControllerRuntimeLog -join ",")
    }
    if ($CrossLanConfirmed) {
        $arguments += "-CrossLanConfirmed"
    }

    $output = & powershell.exe @arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne $ExpectedExitCode) {
        throw "P0 gate exit code $exitCode did not match expected $ExpectedExitCode. Output: $($output -join ' ')"
    }
    if (-not (Test-Path $OutputPath)) {
        throw "P0 gate did not write expected output: $OutputPath"
    }

    return (Get-Content -Path $OutputPath -Raw | ConvertFrom-Json)
}

function Assert-Equal {
    param(
        [string]$Name,
        [object]$Actual,
        [object]$Expected
    )

    if ([string]$Actual -ne [string]$Expected) {
        throw "$Name expected '$Expected' but got '$Actual'"
    }
}

function Assert-True {
    param(
        [string]$Name,
        [bool]$Condition
    )

    if (-not $Condition) {
        throw "$Name expected true"
    }
}

function Add-CaseResult {
    param(
        [System.Collections.ArrayList]$Results,
        [string]$Name,
        [bool]$Passed,
        [string]$Detail
    )

    [void]$Results.Add([pscustomobject]@{
        name = $Name
        passed = $Passed
        detail = $Detail
    })
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$summarizerScript = Join-Path $PSScriptRoot "summarize-dht-validation-report.ps1"
if (-not (Test-Path $summarizerScript)) {
    throw "Required summarizer script not found: $summarizerScript"
}

$workflowSummarizerScript = Join-Path $PSScriptRoot "summarize-dht-workflow-result.ps1"
if (-not (Test-Path $workflowSummarizerScript)) {
    throw "Required workflow-result summarizer script not found: $workflowSummarizerScript"
}

$p0GateScript = Join-Path $PSScriptRoot "test-p0-ds07-evidence-gate.ps1"
if (-not (Test-Path $p0GateScript)) {
    throw "Required P0 evidence gate script not found: $p0GateScript"
}

$timestamp = "{0}_{1}" -f (Get-Date -Format "yyyyMMdd_HHmmss_fff"), ([guid]::NewGuid().ToString("N").Substring(0, 6))
if ([System.IO.Path]::IsPathRooted($ReportRoot)) {
    $reportRootAbsolute = $ReportRoot
} else {
    $reportRootAbsolute = Join-Path $repoRoot $ReportRoot
}
$runRoot = Join-Path $reportRootAbsolute ("dht-validation-toolchain-selftest-{0}" -f $timestamp)
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $runRoot "summary.json"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath = Join-Path $repoRoot $OutputPath
}

$caseResults = New-Object System.Collections.ArrayList

try {
    $dryRunDirectory = New-CaseDirectory -Root $runRoot -Name "dry-run-host"
    Write-JsonFile -Path (Join-Path $dryRunDirectory "summary.json") -Value ([pscustomobject]@{
        status = "dry_run"
        role = "host"
        session_code_redacted = "SELF****"
        report_directory = $dryRunDirectory
        stdout = (Join-Path $dryRunDirectory "host.out.log")
        stderr = (Join-Path $dryRunDirectory "host.err.log")
    })
    $dryRunSummary = Invoke-Summarizer `
        -SummarizerScript $summarizerScript `
        -ReportDirectory @($dryRunDirectory) `
        -OutputPath (Join-Path $runRoot "dry-run-summary.json") `
        -ExpectedExitCode 0
    Assert-Equal -Name "dry_run overall_status" -Actual $dryRunSummary.overall_status -Expected "dry_run"
    Assert-Equal -Name "dry_run classification" -Actual $dryRunSummary.reports[0].classification -Expected "dry_run_no_runtime"
    Add-CaseResult -Results $caseResults -Name "dry_run_report" -Passed $true -Detail "dry_run_no_runtime"
} catch {
    Add-CaseResult -Results $caseResults -Name "dry_run_report" -Passed $false -Detail $_.Exception.Message
}

try {
    $blockedDirectory = New-CaseDirectory -Root $runRoot -Name "bootstrap-blocked-host"
    $blockedBootstrapPath = Join-Path $blockedDirectory "bootstrap-readiness.json"
    Write-JsonFile -Path $blockedBootstrapPath -Value ([pscustomobject]@{
        overall_status = "blocked_no_usable_bootstrap"
        counts = [pscustomobject]@{
            usable_nodes = 0
            suspicious_nodes = 1
            failed_nodes = 1
        }
    })
    Write-JsonFile -Path (Join-Path $blockedDirectory "summary.json") -Value ([pscustomobject]@{
        status = "bootstrap_blocked"
        role = "host"
        session_code_redacted = "SELF****"
        report_directory = $blockedDirectory
        stdout = (Join-Path $blockedDirectory "host.out.log")
        stderr = (Join-Path $blockedDirectory "host.err.log")
        bootstrap_readiness = [pscustomobject]@{
            enabled = $true
            status = "blocked_no_usable_bootstrap"
            output_path = $blockedBootstrapPath
            exit_code = 2
            usable_nodes = 0
            suspicious_nodes = 1
            failed_nodes = 1
            blocked = $true
            error = ""
        }
    })
    $blockedSummary = Invoke-Summarizer `
        -SummarizerScript $summarizerScript `
        -ReportDirectory @($blockedDirectory) `
        -OutputPath (Join-Path $runRoot "bootstrap-blocked-summary.json") `
        -ExpectedExitCode 1
    Assert-Equal -Name "bootstrap_blocked overall_status" -Actual $blockedSummary.overall_status -Expected "bootstrap_blocked"
    Assert-Equal -Name "bootstrap_blocked classification" -Actual $blockedSummary.reports[0].classification -Expected "bootstrap_blocked"
    Assert-True -Name "bootstrap_blocked flag" -Condition ([bool]$blockedSummary.reports[0].bootstrap_blocked)
    Assert-True -Name "bootstrap_blocked triage hint" -Condition ([string]$blockedSummary.triage_hint -match "DHT bootstrap")
    Add-CaseResult -Results $caseResults -Name "bootstrap_blocked_report" -Passed $true -Detail "bootstrap_blocked"
} catch {
    Add-CaseResult -Results $caseResults -Name "bootstrap_blocked_report" -Passed $false -Detail $_.Exception.Message
}

try {
    $hostDirectory = New-CaseDirectory -Root $runRoot -Name "connected-host"
    $controllerDirectory = New-CaseDirectory -Root $runRoot -Name "connected-controller"
    New-ConnectedRoleReport -Directory $hostDirectory -Role "host" -DhtNodes 101
    New-ConnectedRoleReport -Directory $controllerDirectory -Role "controller" -DhtNodes 202
    $connectedSummaryPath = Join-Path $runRoot "connected-summary.json"
    $connectedSummary = Invoke-Summarizer `
        -SummarizerScript $summarizerScript `
        -ReportDirectory @($hostDirectory, $controllerDirectory) `
        -OutputPath $connectedSummaryPath `
        -ExpectedExitCode 0
    Assert-Equal -Name "connected overall_status" -Actual $connectedSummary.overall_status -Expected "success_connected"
    foreach ($report in @($connectedSummary.reports)) {
        Assert-Equal -Name "$($report.role) classification" -Actual $report.classification -Expected "connected"
        Assert-Equal -Name "$($report.role) bootstrap_status" -Actual $report.bootstrap_status -Expected "warning_suspicious_or_partial_bootstrap"
        Assert-True -Name "$($report.role) bootstrap warning" -Condition (@($report.warnings) -contains "bootstrap_readiness_warning")
        Assert-Equal -Name "$($report.role) publish_success" -Actual $report.publish_success -Expected 2
        Assert-Equal -Name "$($report.role) fetch_hits" -Actual $report.fetch_hits -Expected 2
    }
    Add-CaseResult -Results $caseResults -Name "connected_warning_pair" -Passed $true -Detail "success_connected"
} catch {
    Add-CaseResult -Results $caseResults -Name "connected_warning_pair" -Passed $false -Detail $_.Exception.Message
}

try {
    $classificationCases = @(
        [pscustomobject]@{
            name = "dht_not_reachable"
            expected = "dht_not_reachable"
            reachable = $false
            publish_success = 0
            fetch_hits = 0
            dht_nodes = 0
            remote_dht_exchanged = 0
            dht_signal_published = $false
            remote_description_applied = $false
            stderr = ""
        },
        [pscustomobject]@{
            name = "dht_publish_missing"
            expected = "dht_publish_missing"
            reachable = $true
            publish_success = 0
            fetch_hits = 0
            dht_nodes = 64
            remote_dht_exchanged = 0
            dht_signal_published = $false
            remote_description_applied = $false
            stderr = ""
        },
        [pscustomobject]@{
            name = "dht_remote_record_missing"
            expected = "dht_remote_record_missing"
            reachable = $true
            publish_success = 1
            fetch_hits = 0
            dht_nodes = 128
            remote_dht_exchanged = 0
            dht_signal_published = $true
            remote_description_applied = $false
            stderr = ""
        },
        [pscustomobject]@{
            name = "dht_candidates_missing"
            expected = "dht_candidates_missing"
            reachable = $true
            publish_success = 1
            fetch_hits = 1
            dht_nodes = 160
            remote_dht_exchanged = 0
            dht_signal_published = $true
            remote_description_applied = $true
            stderr = ""
        },
        [pscustomobject]@{
            name = "ice_failed_after_dht_exchange"
            expected = "ice_failed_after_dht_exchange"
            reachable = $true
            publish_success = 1
            fetch_hits = 1
            dht_nodes = 180
            remote_dht_exchanged = 1
            dht_signal_published = $true
            remote_description_applied = $true
            stderr = "Runtime ICE state failed role=controller"
        },
        [pscustomobject]@{
            name = "ice_pending_after_dht_exchange"
            expected = "ice_pending_after_dht_exchange"
            reachable = $true
            publish_success = 1
            fetch_hits = 1
            dht_nodes = 180
            remote_dht_exchanged = 1
            dht_signal_published = $true
            remote_description_applied = $true
            stderr = ""
        }
    )

    foreach ($case in $classificationCases) {
        $caseDirectory = New-CaseDirectory -Root $runRoot -Name ("classification-{0}" -f $case.name)
        New-DhtFailureRoleReport `
            -Directory $caseDirectory `
            -Role "controller" `
            -Case $case.name `
            -DhtReachable ([bool]$case.reachable) `
            -PublishSuccess ([int]$case.publish_success) `
            -FetchHits ([int]$case.fetch_hits) `
            -DhtNodes ([int]$case.dht_nodes) `
            -RemoteDhtExchanged ([int]$case.remote_dht_exchanged) `
            -DhtSignalPublished ([bool]$case.dht_signal_published) `
            -RemoteDescriptionApplied ([bool]$case.remote_description_applied) `
            -StderrLine ([string]$case.stderr)
        $caseSummary = Invoke-Summarizer `
            -SummarizerScript $summarizerScript `
            -ReportDirectory @($caseDirectory) `
            -OutputPath (Join-Path $runRoot ("classification-{0}-summary.json" -f $case.name)) `
            -ExpectedExitCode 1
        Assert-Equal -Name "$($case.name) overall_status" -Actual $caseSummary.overall_status -Expected $case.expected
        Assert-Equal -Name "$($case.name) classification" -Actual $caseSummary.reports[0].classification -Expected $case.expected
        Assert-True -Name "$($case.name) raw dht stats" -Condition (-not [string]::IsNullOrWhiteSpace([string]$caseSummary.reports[0].last_dht_stats))
        Assert-True -Name "$($case.name) raw backend diagnostics" -Condition (-not [string]::IsNullOrWhiteSpace([string]$caseSummary.reports[0].last_dht_backend_diagnostics))
        Assert-True -Name "$($case.name) triage hint" -Condition (-not [string]::IsNullOrWhiteSpace([string]$caseSummary.reports[0].triage_hint))
        if ($case.name -eq "ice_failed_after_dht_exchange") {
            Assert-True -Name "ice failure hint shifts away from DHT" -Condition ([string]$caseSummary.triage_hint -match "DHT rendezvous succeeded")
        }
    }
    Add-CaseResult -Results $caseResults -Name "dht_failure_classifications" -Passed $true -Detail "dht_not_reachable,dht_publish_missing,dht_remote_record_missing,dht_candidates_missing,ice_failed,ice_pending"
} catch {
    Add-CaseResult -Results $caseResults -Name "dht_failure_classifications" -Passed $false -Detail $_.Exception.Message
}

try {
    $workflowSuccessPath = Join-Path $runRoot "workflow-success.json"
    $workflowHostSuccessPath = Join-Path $runRoot "workflow-success-host.json"
    $workflowSuccessSummaryPath = Join-Path $runRoot "workflow-success-summary.json"
    Write-JsonFile -Path $workflowHostSuccessPath -Value ([pscustomobject]@{
        schema = "redclaw.dht.role.validation.workflow.v1"
        generated_at = (Get-Date).ToString("o")
        status = "success"
        role = "host"
        mode = "live"
        session_code_redacted = "SELF****"
        report_directory = $hostDirectory
        summary = $connectedSummaryPath
        archive = $null
        archive_zip = $null
        archive_zip_sha256 = ""
        helper_exit_code = 0
        summary_exit_code = 0
        archive_exit_code = 0
        error_message = $null
        toolchain_self_test_included = $false
        existing_report_regression_included = $false
    })
    Write-JsonFile -Path $workflowSuccessPath -Value ([pscustomobject]@{
        schema = "redclaw.dht.role.validation.workflow.v1"
        generated_at = (Get-Date).ToString("o")
        status = "success"
        role = "controller"
        mode = "live"
        session_code_redacted = "SELF****"
        report_directory = $controllerDirectory
        summary = $connectedSummaryPath
        archive = $null
        archive_zip = $null
        archive_zip_sha256 = ""
        helper_exit_code = 0
        summary_exit_code = 0
        archive_exit_code = 0
        error_message = $null
        toolchain_self_test_included = $false
        existing_report_regression_included = $false
    })
    $workflowSuccessSummary = Invoke-WorkflowSummarizer `
        -WorkflowSummarizerScript $workflowSummarizerScript `
        -WorkflowResult @($workflowHostSuccessPath, $workflowSuccessPath) `
        -OutputPath $workflowSuccessSummaryPath `
        -ExpectedExitCode 0
    Assert-Equal -Name "workflow success overall_status" -Actual $workflowSuccessSummary.overall_status -Expected "success_connected"
    Assert-Equal -Name "workflow success classification" -Actual $workflowSuccessSummary.workflows[0].classification -Expected "success_connected"
    Assert-Equal -Name "workflow success summary_source" -Actual $workflowSuccessSummary.workflows[0].summary_source -Expected "workflow_summary"
    Assert-Equal -Name "workflow success role_scope" -Actual $workflowSuccessSummary.role_coverage.workflow_scope -Expected "host_controller_cli_pair"
    Assert-Equal -Name "workflow success p0 gate" -Actual $workflowSuccessSummary.p0_ds07_gate_status -Expected "cli_pair_connected_ui_playback_required"
    Assert-True -Name "workflow success triage hint" -Condition ([string]$workflowSuccessSummary.triage_hint -match "UI playback")
    Assert-True -Name "workflow success raw dht stats" -Condition (-not [string]::IsNullOrWhiteSpace([string]$workflowSuccessSummary.workflows[0].report_classifications[0].last_dht_stats))
    Assert-True -Name "workflow success raw backend diagnostics" -Condition (-not [string]::IsNullOrWhiteSpace([string]$workflowSuccessSummary.workflows[0].report_classifications[0].last_dht_backend_diagnostics))
    $workflowConsoleOutput = Invoke-WorkflowSummarizerConsole `
        -WorkflowSummarizerScript $workflowSummarizerScript `
        -WorkflowResult @($workflowHostSuccessPath, $workflowSuccessPath) `
        -OutputPath (Join-Path $runRoot "workflow-success-console-summary.json") `
        -ExpectedExitCode 0
    Assert-True -Name "workflow console remote_dht_exchanged" -Condition ($workflowConsoleOutput.Contains("remote_dht_exchanged=1"))
    Assert-True -Name "workflow console local_direct" -Condition ($workflowConsoleOutput.Contains("local_direct=3"))
    Assert-True -Name "workflow console local_full" -Condition ($workflowConsoleOutput.Contains("local_full=True"))
    Assert-True -Name "workflow console remote_latest" -Condition ($workflowConsoleOutput.Contains("remote_latest=5"))
    Assert-True -Name "workflow console triage hint" -Condition ($workflowConsoleOutput.Contains("triage_hint: Host/Controller CLI connection succeeded"))
    Add-CaseResult -Results $caseResults -Name "workflow_result_success" -Passed $true -Detail "success_connected"
} catch {
    Add-CaseResult -Results $caseResults -Name "workflow_result_success" -Passed $false -Detail $_.Exception.Message
}

try {
    $p0MissingUiSummary = Invoke-P0Gate `
        -P0GateScript $p0GateScript `
        -WorkflowSummary $workflowSuccessSummaryPath `
        -UiEvidence @() `
        -HostRuntimeLog @() `
        -ControllerRuntimeLog @() `
        -OutputPath (Join-Path $runRoot "p0-gate-missing-ui-summary.json") `
        -ExpectedExitCode 1 `
        -CrossLanConfirmed
    Assert-Equal -Name "p0 missing ui status" -Actual $p0MissingUiSummary.status -Expected "fail"
    Assert-Equal -Name "p0 missing ui acceptance" -Actual $p0MissingUiSummary.p0_ds07_acceptance_status -Expected "not_ready"
    $uiCheck = @($p0MissingUiSummary.checks | Where-Object { $_.name -eq "ui_playback_evidence" }) | Select-Object -First 1
    Assert-Equal -Name "p0 missing ui check" -Actual $uiCheck.passed -Expected $false
    Add-CaseResult -Results $caseResults -Name "p0_gate_missing_ui_evidence" -Passed $true -Detail "not_ready"
} catch {
    Add-CaseResult -Results $caseResults -Name "p0_gate_missing_ui_evidence" -Passed $false -Detail $_.Exception.Message
}

try {
    $p0UiEvidencePath = Join-Path $runRoot "synthetic-ui-playback.png"
    "synthetic ui evidence" | Set-Content -Path $p0UiEvidencePath -Encoding UTF8

    $p0PassSummary = Invoke-P0Gate `
        -P0GateScript $p0GateScript `
        -WorkflowSummary $workflowSuccessSummaryPath `
        -UiEvidence @($p0UiEvidencePath) `
        -HostRuntimeLog @() `
        -ControllerRuntimeLog @() `
        -OutputPath (Join-Path $runRoot "p0-gate-pass-summary.json") `
        -ExpectedExitCode 0 `
        -CrossLanConfirmed
    Assert-Equal -Name "p0 pass status" -Actual $p0PassSummary.status -Expected "pass"
    Assert-Equal -Name "p0 pass acceptance" -Actual $p0PassSummary.p0_ds07_acceptance_status -Expected "ready_for_handoff_review"
    Assert-Equal -Name "p0 pass host captured" -Actual $p0PassSummary.host_stream_stats.captured -Expected 12
    Assert-Equal -Name "p0 pass controller GUI presented" -Actual $p0PassSummary.controller_gui_stats.presented -Expected 10
    Add-CaseResult -Results $caseResults -Name "p0_gate_ready_with_ui_and_stats" -Passed $true -Detail "ready_for_handoff_review"
} catch {
    Add-CaseResult -Results $caseResults -Name "p0_gate_ready_with_ui_and_stats" -Passed $false -Detail $_.Exception.Message
}

try {
    $singleControllerSummaryPath = Join-Path $runRoot "single-controller-summary.json"
    Invoke-Summarizer `
        -SummarizerScript $summarizerScript `
        -ReportDirectory @($controllerDirectory) `
        -OutputPath $singleControllerSummaryPath `
        -ExpectedExitCode 0 | Out-Null

    $workflowSingleRolePath = Join-Path $runRoot "workflow-single-role.json"
    Write-JsonFile -Path $workflowSingleRolePath -Value ([pscustomobject]@{
        schema = "redclaw.dht.role.validation.workflow.v1"
        generated_at = (Get-Date).ToString("o")
        status = "success"
        role = "controller"
        mode = "live"
        session_code_redacted = "SELF****"
        report_directory = $controllerDirectory
        summary = $singleControllerSummaryPath
        archive = $null
        archive_zip = $null
        archive_zip_sha256 = ""
        helper_exit_code = 0
        summary_exit_code = 0
        archive_exit_code = 0
        error_message = $null
        toolchain_self_test_included = $false
        existing_report_regression_included = $false
    })
    $workflowSingleRoleSummary = Invoke-WorkflowSummarizer `
        -WorkflowSummarizerScript $workflowSummarizerScript `
        -WorkflowResult @($workflowSingleRolePath) `
        -OutputPath (Join-Path $runRoot "workflow-single-role-summary.json") `
        -ExpectedExitCode 0
    Assert-Equal -Name "workflow single role overall_status" -Actual $workflowSingleRoleSummary.overall_status -Expected "success_connected"
    Assert-Equal -Name "workflow single role role_scope" -Actual $workflowSingleRoleSummary.role_coverage.workflow_scope -Expected "single_role_cli"
    Assert-Equal -Name "workflow single role p0 gate" -Actual $workflowSingleRoleSummary.p0_ds07_gate_status -Expected "single_role_cli_connected_peer_result_required"
    Assert-True -Name "workflow single role missing host" -Condition (@($workflowSingleRoleSummary.role_coverage.missing_roles) -contains "host")
    Add-CaseResult -Results $caseResults -Name "workflow_result_single_role_gate" -Passed $true -Detail "single_role_cli"
} catch {
    Add-CaseResult -Results $caseResults -Name "workflow_result_single_role_gate" -Passed $false -Detail $_.Exception.Message
}

try {
    $workflowMissingRuntimePath = Join-Path $runRoot "workflow-runtime-missing.json"
    Write-JsonFile -Path $workflowMissingRuntimePath -Value ([pscustomobject]@{
        schema = "redclaw.dht.role.validation.workflow.v1"
        generated_at = (Get-Date).ToString("o")
        status = "helper_failed"
        role = "controller"
        mode = "dry_run"
        session_code_redacted = "SELF****"
        report_directory = $null
        summary = (Join-Path $runRoot "missing-workflow-summary.json")
        archive = $null
        archive_zip = $null
        archive_zip_sha256 = ""
        helper_exit_code = 1
        summary_exit_code = $null
        archive_exit_code = $null
        error_message = "Runtime executable not found: synthetic.exe"
        toolchain_self_test_included = $false
        existing_report_regression_included = $false
    })
    $workflowMissingRuntimeSummary = Invoke-WorkflowSummarizer `
        -WorkflowSummarizerScript $workflowSummarizerScript `
        -WorkflowResult @($workflowMissingRuntimePath) `
        -OutputPath (Join-Path $runRoot "workflow-runtime-missing-summary.json") `
        -ExpectedExitCode 1
    Assert-Equal -Name "workflow runtime missing overall_status" -Actual $workflowMissingRuntimeSummary.overall_status -Expected "runtime_missing"
    Assert-Equal -Name "workflow runtime missing classification" -Actual $workflowMissingRuntimeSummary.workflows[0].classification -Expected "runtime_missing"
    Assert-True -Name "workflow runtime missing triage hint" -Condition ([string]$workflowMissingRuntimeSummary.triage_hint -match "Runtime executable is missing")
    Add-CaseResult -Results $caseResults -Name "workflow_result_runtime_missing" -Passed $true -Detail "runtime_missing"
} catch {
    Add-CaseResult -Results $caseResults -Name "workflow_result_runtime_missing" -Passed $false -Detail $_.Exception.Message
}

try {
    $iceWorkflowReportDirectory = New-CaseDirectory -Root $runRoot -Name "workflow-ice-failed-report"
    New-DhtFailureRoleReport `
        -Directory $iceWorkflowReportDirectory `
        -Role "controller" `
        -Case "workflow_ice_failed" `
        -DhtReachable $true `
        -PublishSuccess 1 `
        -FetchHits 1 `
        -DhtNodes 180 `
        -RemoteDhtExchanged 1 `
        -DhtSignalPublished $true `
        -RemoteDescriptionApplied $true `
        -StderrLine "Runtime ICE state failed role=controller"

    $iceWorkflowSummaryPath = Join-Path $runRoot "workflow-ice-failed-dht-summary.json"
    Invoke-Summarizer `
        -SummarizerScript $summarizerScript `
        -ReportDirectory @($iceWorkflowReportDirectory) `
        -OutputPath $iceWorkflowSummaryPath `
        -ExpectedExitCode 1 | Out-Null

    $iceWorkflowResultPath = Join-Path $runRoot "workflow-ice-failed-result.json"
    Write-JsonFile -Path $iceWorkflowResultPath -Value ([pscustomobject]@{
        schema = "redclaw.dht.role.validation.workflow.v1"
        generated_at = (Get-Date).ToString("o")
        status = "helper_failed"
        role = "controller"
        mode = "live"
        session_code_redacted = "SELF****"
        report_directory = $iceWorkflowReportDirectory
        summary = $iceWorkflowSummaryPath
        archive = $null
        archive_zip = $null
        archive_zip_sha256 = ""
        helper_exit_code = 1
        summary_exit_code = 1
        archive_exit_code = $null
        error_message = $null
        toolchain_self_test_included = $false
        existing_report_regression_included = $false
    })

    $iceWorkflowResultSummary = Invoke-WorkflowSummarizer `
        -WorkflowSummarizerScript $workflowSummarizerScript `
        -WorkflowResult @($iceWorkflowResultPath) `
        -OutputPath (Join-Path $runRoot "workflow-ice-failed-result-summary.json") `
        -ExpectedExitCode 1
    Assert-Equal -Name "workflow ice failure overall_status" -Actual $iceWorkflowResultSummary.overall_status -Expected "ice_failed_after_dht_exchange"
    Assert-Equal -Name "workflow ice failure classification" -Actual $iceWorkflowResultSummary.workflows[0].classification -Expected "ice_failed_after_dht_exchange"
    Assert-Equal -Name "workflow ice failure report classification" -Actual $iceWorkflowResultSummary.workflows[0].report_classifications[0].classification -Expected "ice_failed_after_dht_exchange"
    Assert-True -Name "workflow ice failure triage hint" -Condition ([string]$iceWorkflowResultSummary.triage_hint -match "DHT rendezvous succeeded")
    Assert-True -Name "workflow ice failure raw dht stats" -Condition (-not [string]::IsNullOrWhiteSpace([string]$iceWorkflowResultSummary.workflows[0].report_classifications[0].last_dht_stats))
    Add-CaseResult -Results $caseResults -Name "workflow_result_ice_failed_summary" -Passed $true -Detail "ice_failed_after_dht_exchange"
} catch {
    Add-CaseResult -Results $caseResults -Name "workflow_result_ice_failed_summary" -Passed $false -Detail $_.Exception.Message
}

if ($IncludeExistingReportRegression) {
    try {
        $existingReport = Join-Path $repoRoot "build\reports\dht-no-mailbox-priority-candidate-20260630_025851"
        if (Test-Path $existingReport) {
            $existingSummary = Invoke-Summarizer `
                -SummarizerScript $summarizerScript `
                -ReportDirectory @($existingReport) `
                -OutputPath (Join-Path $runRoot "existing-report-summary.json") `
                -ExpectedExitCode 0
            Assert-Equal -Name "existing report overall_status" -Actual $existingSummary.overall_status -Expected "success_connected"
            Add-CaseResult -Results $caseResults -Name "existing_report_regression" -Passed $true -Detail $existingReport
        } else {
            Add-CaseResult -Results $caseResults -Name "existing_report_regression" -Passed $true -Detail "skipped: report not found"
        }
    } catch {
        Add-CaseResult -Results $caseResults -Name "existing_report_regression" -Passed $false -Detail $_.Exception.Message
    }
}

$failed = @($caseResults | Where-Object { -not $_.passed })
$result = [pscustomobject]@{
    schema = "redclaw.dht.validation.toolchain.selftest.v1"
    generated_at = (Get-Date).ToString("o")
    overall_status = if ($failed.Count -eq 0) { "pass" } else { "fail" }
    run_root = $runRoot
    case_count = $caseResults.Count
    failure_count = $failed.Count
    cases = @($caseResults)
}

Write-JsonFile -Path $OutputPath -Value $result

if ($Json) {
    $result | ConvertTo-Json -Depth 8
} else {
    Write-Host "[dht-toolchain-test] status: $($result.overall_status)"
    Write-Host "[dht-toolchain-test] report: $OutputPath"
    foreach ($case in $caseResults) {
        Write-Host ("[dht-toolchain-test] {0}: {1} ({2})" -f $case.name, $(if ($case.passed) { "pass" } else { "fail" }), $case.detail)
    }
}

if ($failed.Count -gt 0) {
    exit 1
}

exit 0
