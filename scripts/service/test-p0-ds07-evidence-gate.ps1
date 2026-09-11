param(
    [Parameter(Mandatory = $true)]
    [string]$WorkflowSummary,

    [string[]]$UiEvidence = @(),

    [string[]]$HostRuntimeLog = @(),

    [string[]]$ControllerRuntimeLog = @(),

    [switch]$CrossLanConfirmed,

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

function Resolve-InputFile {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path)) {
        return $null
    }

    $item = Get-Item -Path $Path
    if ($item.PSIsContainer) {
        return $null
    }

    return $item
}

function Get-JsonFile {
    param([string]$Path)

    $item = Resolve-InputFile -Path $Path
    if ($null -eq $item) {
        throw "JSON file not found: $Path"
    }

    return Get-Content -Path $item.FullName -Raw | ConvertFrom-Json
}

function New-Check {
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Detail
    )

    return [pscustomobject]@{
        name = $Name
        passed = $Passed
        detail = $Detail
    }
}

function Get-LastStreamStatsLine {
    param(
        [string[]]$Paths,
        [string]$Role
    )

    foreach ($path in (Expand-ArgumentList -Values $Paths)) {
        $item = Resolve-InputFile -Path $path
        if ($null -eq $item) {
            continue
        }

        $line = Get-Content -Path $item.FullName |
            Where-Object { $_ -match ("Runtime desktop stream stats role={0}\b" -f [regex]::Escape($Role)) } |
            Select-Object -Last 1
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            return [pscustomobject]@{
                path = $item.FullName
                line = [string]$line
            }
        }
    }

    return $null
}

function Get-LastGuiStatsLine {
    param([string[]]$Paths)

    foreach ($path in (Expand-ArgumentList -Values $Paths)) {
        $item = Resolve-InputFile -Path $path
        if ($null -eq $item) {
            continue
        }
        $line = Get-Content -Path $item.FullName |
            Where-Object { $_ -match 'GUI direct frame stats ' } |
            Select-Object -Last 1
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            return [pscustomobject]@{ path = $item.FullName; line = [string]$line }
        }
    }
    return $null
}

function Get-RuntimeLogCandidatesFromWorkflow {
    param(
        [object]$WorkflowSummary,
        [string]$Role
    )

    $paths = @()
    foreach ($workflow in @($WorkflowSummary.workflows)) {
        $workflowRole = [string]$workflow.role
        if ($workflowRole -ne $Role) {
            continue
        }

        $reportDirectory = [string]$workflow.report_directory
        if ([string]::IsNullOrWhiteSpace($reportDirectory) -or -not (Test-Path $reportDirectory)) {
            continue
        }

        $summaryPath = Join-Path $reportDirectory "summary.json"
        if (Test-Path $summaryPath) {
            try {
                $summary = Get-Content -Path $summaryPath -Raw | ConvertFrom-Json
                if (-not [string]::IsNullOrWhiteSpace([string]$summary.stdout)) {
                    $paths += [string]$summary.stdout
                }
            } catch {
                # Ignore malformed helper summaries; the gate will report missing stats if no usable log is found.
            }
        }

        $roleLog = Join-Path $reportDirectory ("{0}.out.log" -f $Role)
        if (Test-Path $roleLog) {
            $paths += $roleLog
        }
    }

    return @($paths | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
}

function ConvertFrom-StatsLine {
    param([string]$Line)

    $values = @{}
    foreach ($match in [regex]::Matches($Line, '([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)')) {
        $values[$match.Groups[1].Value] = $match.Groups[2].Value
    }

    return $values
}

function Get-IntegerField {
    param(
        [hashtable]$Fields,
        [string]$Name,
        [int]$Default = 0
    )

    if (-not $Fields.ContainsKey($Name)) {
        return $Default
    }

    $value = 0
    if ([int]::TryParse([string]$Fields[$Name], [ref]$value)) {
        return $value
    }

    return $Default
}

function Test-HostStats {
    param([hashtable]$Fields)

    $failed = @()
    if ([string]$Fields["channel_open"] -ne "true") {
        $failed += "channel_open"
    }
    if ((Get-IntegerField -Fields $Fields -Name "captured") -le 0) {
        $failed += "captured"
    }
    if ((Get-IntegerField -Fields $Fields -Name "encoded") -le 0) {
        $failed += "encoded"
    }
    if ((Get-IntegerField -Fields $Fields -Name "transmitted") -le 0) {
        $failed += "transmitted"
    }
    if ((Get-IntegerField -Fields $Fields -Name "encode_failures") -ne 0) {
        $failed += "encode_failures"
    }

    return $failed
}

function Test-ControllerStats {
    param([hashtable]$Fields)

    $failed = @()
    if ([string]$Fields["channel_open"] -ne "true") {
        $failed += "channel_open"
    }
    if ((Get-IntegerField -Fields $Fields -Name "received") -le 0) {
        $failed += "received"
    }
    if ((Get-IntegerField -Fields $Fields -Name "media_fragments_received") -le 0) {
        $failed += "media_fragments_received"
    }
    if ((Get-IntegerField -Fields $Fields -Name "encoded_frames_reassembled") -le 0) {
        $failed += "encoded_frames_reassembled"
    }
    if ((Get-IntegerField -Fields $Fields -Name "direct_pipe_written") -le 0) {
        $failed += "direct_pipe_written"
    }
    if ((Get-IntegerField -Fields $Fields -Name "decoded") -ne 0) {
        $failed += "runtime_decoded_must_be_zero"
    }
    if ((Get-IntegerField -Fields $Fields -Name "rendered") -ne 0) {
        $failed += "runtime_rendered_must_be_zero"
    }
    if ((Get-IntegerField -Fields $Fields -Name "decode_failures") -ne 0) {
        $failed += "decode_failures"
    }
    if ((Get-IntegerField -Fields $Fields -Name "render_failures") -ne 0) {
        $failed += "render_failures"
    }

    return $failed
}

function Test-GuiStats {
    param([hashtable]$Fields)

    $failed = @()
    if ((Get-IntegerField -Fields $Fields -Name "gui_decode_success_total") -le 0) {
        $failed += "gui_decode_success_total"
    }
    if ((Get-IntegerField -Fields $Fields -Name "presented_total") -le 0) {
        $failed += "presented_total"
    }
    if ((Get-IntegerField -Fields $Fields -Name "decode_failures") -ne 0) {
        $failed += "gui_decode_failures"
    }
    if ((Get-IntegerField -Fields $Fields -Name "present_failures") -ne 0) {
        $failed += "gui_present_failures"
    }
    return $failed
}

function Get-UiEvidence {
    param([string[]]$Paths)

    $items = @()
    foreach ($path in (Expand-ArgumentList -Values $Paths)) {
        $item = Resolve-InputFile -Path $path
        if ($null -eq $item) {
            $items += [pscustomobject]@{
                path = $path
                exists = $false
                length = 0
            }
            continue
        }

        $items += [pscustomobject]@{
            path = $item.FullName
            exists = $true
            length = $item.Length
        }
    }

    return @($items)
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

$workflow = Get-JsonFile -Path $WorkflowSummary
$checks = @()

$checks += New-Check `
    -Name "workflow_summary_connected" `
    -Passed ([string]$workflow.overall_status -eq "success_connected") `
    -Detail ([string]$workflow.overall_status)

$checks += New-Check `
    -Name "workflow_cli_pair_gate" `
    -Passed ([string]$workflow.p0_ds07_gate_status -eq "cli_pair_connected_ui_playback_required") `
    -Detail ([string]$workflow.p0_ds07_gate_status)

$checks += New-Check `
    -Name "cross_lan_confirmed" `
    -Passed ([bool]$CrossLanConfirmed) `
    -Detail $(if ($CrossLanConfirmed) { "confirmed_by_operator" } else { "not_confirmed" })

$uiEvidenceItems = Get-UiEvidence -Paths $UiEvidence
$validUiEvidence = @($uiEvidenceItems | Where-Object { $_.exists -and $_.length -gt 0 })
$checks += New-Check `
    -Name "ui_playback_evidence" `
    -Passed ($validUiEvidence.Count -gt 0) `
    -Detail $(if ($validUiEvidence.Count -gt 0) { ($validUiEvidence[0].path) } else { "missing" })

$hostLogCandidates = @(Expand-ArgumentList -Values $HostRuntimeLog) + @(Get-RuntimeLogCandidatesFromWorkflow -WorkflowSummary $workflow -Role "host")
$controllerLogCandidates = @(Expand-ArgumentList -Values $ControllerRuntimeLog) + @(Get-RuntimeLogCandidatesFromWorkflow -WorkflowSummary $workflow -Role "controller")

$hostStatsRecord = Get-LastStreamStatsLine -Paths $hostLogCandidates -Role "host"
$hostFields = @{}
$hostFailures = @("missing_stats")
if ($null -ne $hostStatsRecord) {
    $hostFields = ConvertFrom-StatsLine -Line $hostStatsRecord.line
    $hostFailures = Test-HostStats -Fields $hostFields
}
$checks += New-Check `
    -Name "host_stream_stats" `
    -Passed ($hostFailures.Count -eq 0) `
    -Detail $(if ($hostFailures.Count -eq 0) { $hostStatsRecord.path } else { ($hostFailures -join ",") })

$controllerStatsRecord = Get-LastStreamStatsLine -Paths $controllerLogCandidates -Role "controller"
$controllerFields = @{}
$controllerFailures = @("missing_stats")
if ($null -ne $controllerStatsRecord) {
    $controllerFields = ConvertFrom-StatsLine -Line $controllerStatsRecord.line
    $controllerFailures = Test-ControllerStats -Fields $controllerFields
}
$checks += New-Check `
    -Name "controller_stream_stats" `
    -Passed ($controllerFailures.Count -eq 0) `
    -Detail $(if ($controllerFailures.Count -eq 0) { $controllerStatsRecord.path } else { ($controllerFailures -join ",") })

$guiStatsRecord = Get-LastGuiStatsLine -Paths $controllerLogCandidates
$guiFields = @{}
$guiFailures = @("missing_gui_stats")
if ($null -ne $guiStatsRecord) {
    $guiFields = ConvertFrom-StatsLine -Line $guiStatsRecord.line
    $guiFailures = Test-GuiStats -Fields $guiFields
}
$checks += New-Check `
    -Name "controller_gui_playback_stats" `
    -Passed ($guiFailures.Count -eq 0) `
    -Detail $(if ($guiFailures.Count -eq 0) { $guiStatsRecord.path } else { ($guiFailures -join ",") })

$failedChecks = @($checks | Where-Object { -not $_.passed })
$result = [pscustomobject]@{
    schema = "redclaw.p0.ds07.evidence.gate.v1"
    generated_at = (Get-Date).ToString("o")
    status = if ($failedChecks.Count -eq 0) { "pass" } else { "fail" }
    p0_ds07_acceptance_status = if ($failedChecks.Count -eq 0) { "ready_for_handoff_review" } else { "not_ready" }
    workflow_summary = (Resolve-InputFile -Path $WorkflowSummary).FullName
    workflow_overall_status = [string]$workflow.overall_status
    workflow_gate_status = [string]$workflow.p0_ds07_gate_status
    cross_lan_confirmed = [bool]$CrossLanConfirmed
    ui_evidence = @($uiEvidenceItems)
    host_stream_stats = [pscustomobject]@{
        log_candidates = @($hostLogCandidates)
        path = if ($null -eq $hostStatsRecord) { $null } else { $hostStatsRecord.path }
        line = if ($null -eq $hostStatsRecord) { $null } else { $hostStatsRecord.line }
        captured = Get-IntegerField -Fields $hostFields -Name "captured"
        encoded = Get-IntegerField -Fields $hostFields -Name "encoded"
        transmitted = Get-IntegerField -Fields $hostFields -Name "transmitted"
        encode_failures = Get-IntegerField -Fields $hostFields -Name "encode_failures"
        failures = @($hostFailures)
    }
    controller_stream_stats = [pscustomobject]@{
        log_candidates = @($controllerLogCandidates)
        path = if ($null -eq $controllerStatsRecord) { $null } else { $controllerStatsRecord.path }
        line = if ($null -eq $controllerStatsRecord) { $null } else { $controllerStatsRecord.line }
        received = Get-IntegerField -Fields $controllerFields -Name "received"
        media_fragments_received = Get-IntegerField -Fields $controllerFields -Name "media_fragments_received"
        encoded_frames_reassembled = Get-IntegerField -Fields $controllerFields -Name "encoded_frames_reassembled"
        direct_pipe_written = Get-IntegerField -Fields $controllerFields -Name "direct_pipe_written"
        decoded = Get-IntegerField -Fields $controllerFields -Name "decoded"
        rendered = Get-IntegerField -Fields $controllerFields -Name "rendered"
        decode_failures = Get-IntegerField -Fields $controllerFields -Name "decode_failures"
        render_failures = Get-IntegerField -Fields $controllerFields -Name "render_failures"
        failures = @($controllerFailures)
    }
    controller_gui_stats = [pscustomobject]@{
        path = if ($null -eq $guiStatsRecord) { $null } else { $guiStatsRecord.path }
        line = if ($null -eq $guiStatsRecord) { $null } else { $guiStatsRecord.line }
        decoded = Get-IntegerField -Fields $guiFields -Name "gui_decode_success_total"
        presented = Get-IntegerField -Fields $guiFields -Name "presented_total"
        decode_failures = Get-IntegerField -Fields $guiFields -Name "decode_failures"
        present_failures = Get-IntegerField -Fields $guiFields -Name "present_failures"
        failures = @($guiFailures)
    }
    checks = @($checks)
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
    Write-Host ("[p0-ds07-gate] status: {0}" -f $result.status)
    Write-Host ("[p0-ds07-gate] acceptance: {0}" -f $result.p0_ds07_acceptance_status)
    foreach ($check in @($checks)) {
        Write-Host ("[p0-ds07-gate] {0}: {1} ({2})" -f $check.name, $(if ($check.passed) { "pass" } else { "fail" }), $check.detail)
    }
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        Write-Host ("[p0-ds07-gate] wrote: {0}" -f $OutputPath)
    }
}

if ($failedChecks.Count -gt 0) {
    exit 1
}

exit 0
