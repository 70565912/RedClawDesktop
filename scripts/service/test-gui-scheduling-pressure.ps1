param(
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [Parameter(Mandatory=$true)][string]$ReportName,
    [int]$Repetitions = 3,
    [int]$IdleSeconds = 30,
    [switch]$RealInputs,
    [switch]$HiddenPanel
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$run = Get-Content -Raw -LiteralPath (Join-Path $RunDirectory 'result.json') | ConvertFrom-Json
if (-not $run.overall_passed -or $run.signal_transport -ne 'dht') { throw 'Real public-DHT media gate required.' }
$report = Join-Path $RunDirectory $ReportName
if (Test-Path -LiteralPath $report) { throw 'Preserve existing measurement evidence.' }
New-Item -ItemType Directory -Path $report | Out-Null
$qa = Join-Path $PSScriptRoot 'invoke-cross-lan-debug-control.ps1'
$sampleNumber = 0
function Invoke-Qa([string]$Role, [string]$Action) {
    $reply = & $qa -ControlName ('RedClawDesktop.LocalGui.'+$Role+'.'+$run.run_id) `
        -Action $Action -TimeoutMs 20000 -Json | ConvertFrom-Json
    if (-not $reply.ok) {
        $reply | ConvertTo-Json -Depth 64 | Set-Content -LiteralPath (Join-Path $report ('failure-'+$Role+'-'+$Action+'-'+[guid]::NewGuid().ToString('N')+'.json')) -Encoding UTF8
        throw ($Role+'/'+$Action+': '+$reply.error_code+' '+$reply.error_detail)
    }
    return $reply
}
function Snapshot([string]$Tag) {
    $pair = @{}
    foreach ($role in 'Host','Controller') {
        $pair[$role] = Invoke-Qa $role 'agent_status'
        $pair[$role] | ConvertTo-Json -Depth 64 -Compress | Set-Content -LiteralPath `
            (Join-Path $report ($Tag+'-'+$role+'.json')) -Encoding UTF8
    }
    return $pair
}
function Arm-Trace([string]$Tag) {
    if ($RealInputs) {
        # Task/approval UI can move local focus between windows. Reacquire the
        # existing consented lease and owned canvas before the measurement starts.
        [void](Invoke-Qa 'Controller' 'remote_control_pause')
        [void](Invoke-Qa 'Controller' 'remote_control_start')
        Start-Sleep -Milliseconds 500
    }
    foreach ($role in 'Host','Controller') {
        Invoke-Qa $role 'measurement_arm' | ConvertTo-Json -Depth 32 |
            Set-Content -LiteralPath (Join-Path $report ($Tag+'-'+$role+'-trace-arm.json')) -Encoding UTF8
    }
    if ($RealInputs) {
        Invoke-Qa 'Controller' 'input_probe_start' | ConvertTo-Json -Depth 32 |
            Set-Content -LiteralPath (Join-Path $report ($Tag+'-input-start.json')) -Encoding UTF8
    }
}
function Measure-Window([string]$Tag, [int]$Seconds, [bool]$UntilComplete=$false, $First=$null) {
    if ($null -eq $First) { Arm-Trace $Tag }
    $first = if ($null -ne $First) {$First} else {Snapshot ($Tag+'-start')}
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $samples = [Collections.Generic.List[object]]::new()
    $last = $first
    $observationGapUs = 0L
    $commonDurationUs = 0L
    do {
        Start-Sleep -Milliseconds 1000
        $previousUs = $last.Controller.status.gui_scheduling.sampled_at_us
        $last = Snapshot ($Tag+'-'+($script:sampleNumber++))
        $observationGapUs = $last.Controller.status.gui_scheduling.sampled_at_us - $previousUs
        $samples.Add([pscustomobject]@{elapsed_ms=$clock.ElapsedMilliseconds;Host=$last.Host;Controller=$last.Controller})
        # The shared interval starts after BOTH initial snapshots and ends
        # before EITHER final snapshot. Serial QA round trips must not shorten
        # a requested 30-second idle/recovery window below 30 seconds.
        $commonDurationUs = [math]::Min($last.Host.status.gui_scheduling.sampled_at_us,
            $last.Controller.status.gui_scheduling.sampled_at_us) -
            [math]::Max($first.Host.status.gui_scheduling.sampled_at_us,
                $first.Controller.status.gui_scheduling.sampled_at_us)
        if ($UntilComplete -and $last.Host.result.task_state -eq 'completed' -and
            $last.Controller.result.task_state -eq 'completed' -and
            $last.Host.result.task_timing.terminal_painted_us -gt 0 -and
            (($HiddenPanel -and $last.Controller.result.task_timing.terminal_consumed_us -gt 0) -or
             $last.Controller.result.task_timing.terminal_painted_us -gt 0)) { break }
    } while ($(if($UntilComplete){$clock.Elapsed.TotalSeconds -lt $Seconds}else{$commonDurationUs -lt $Seconds*1000000L}))
    $completed = $last.Host.result.task_state -eq 'completed' -and $last.Controller.result.task_state -eq 'completed'
    $duration = ($last.Controller.status.gui_scheduling.sampled_at_us - $first.Controller.status.gui_scheduling.sampled_at_us) / 1000000.0
    if ($duration -le 0) { throw 'Missing monotonic GUI snapshot timestamps.' }
    $stages = [ordered]@{}
    foreach ($stage in $last.Controller.status.gui_scheduling.totals.PSObject.Properties) {
        $before = $first.Controller.status.gui_scheduling.totals.($stage.Name)
        $after = $stage.Value
        $count = [long]$after.count - [long]$before.count
        if ($count -le 0) { continue }
        $histogram = @{}
        foreach ($bin in $after.histogram.PSObject.Properties) {
            $delta = [long]$bin.Value - [long]$before.histogram.($bin.Name)
            if ($delta -gt 0) { $histogram[[int]$bin.Name] = $delta }
        }
        $quantiles = @{}
        foreach ($p in 50,95,99) {
            $rank = [math]::Ceiling($count*$p/100.0); $seen = 0L
            foreach ($bin in @($histogram.Keys | Sort-Object)) {
                $seen += $histogram[$bin]
                if ($seen -ge $rank) {
                    $base = [math]::Pow(2,[math]::Floor($bin/4))
                    $quantiles["p${p}_us_upper_bound"] = $base + [math]::Max(1,$base/4)*($bin%4+1)-1
                    break
                }
            }
        }
        $maxBin = ($histogram.Keys | Measure-Object -Maximum).Maximum
        $maxBase = [math]::Pow(2,[math]::Floor($maxBin/4))
        $stages[$stage.Name] = @{count=$count;average_us=([long]$after.total_us-[long]$before.total_us)/$count;
            max_us_upper_bound=$maxBase+[math]::Max(1,$maxBase/4)*($maxBin%4+1)-1;percentiles=$quantiles}
    }
    $traces = @{}
    foreach ($role in 'Host','Controller') {
        $export = Invoke-Qa $role 'measurement_export'
        $metadataPath = Join-Path $report ($Tag+'-'+$role+'-trace-export.json')
        $export | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $metadataPath -Encoding UTF8
        $traces[$role] = @{metadata_path=$metadataPath;trace_path=$export.result.trace_path}
    }
    $inputEvidence = $null
    if ($RealInputs) {
        [void](Invoke-Qa 'Controller' 'input_probe_stop')
        Start-Sleep -Milliseconds 300
        $inputEvidence = Invoke-Qa 'Controller' 'input_probe_export'
        $inputEvidence | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath (Join-Path $report ($Tag+'-input-export.json')) -Encoding UTF8
        $runtimeStages = Join-Path (Join-Path $RunDirectory 'controller') ('input-injections-'+$inputEvidence.result.generation+'.json')
        $exportWait = [Diagnostics.Stopwatch]::StartNew()
        while (-not (Test-Path -LiteralPath $runtimeStages) -and $exportWait.Elapsed.TotalSeconds -lt 10) { Start-Sleep -Milliseconds 100 }
        if (-not (Test-Path -LiteralPath $runtimeStages)) { throw 'Controller runtime stage export did not finish.' }
    }
    $result = [pscustomobject]@{window=$Tag;seconds=$duration;observations=$samples.Count;traces=$traces;input_evidence=$inputEvidence;
        completed_both=$completed;terminal_observation_uncertainty_ms=$observationGapUs/1000;
        decode_fps=($last.Controller.status.stream.gui_decode_success-$first.Controller.status.stream.gui_decode_success)/$duration;
        present_fps=($last.Controller.status.gui_scheduling.totals.displayed_frame.count-$first.Controller.status.gui_scheduling.totals.displayed_frame.count)/$duration;
        present_counter_source='gui_scheduling.displayed_frame';
        decode_counter_source='periodically_published_runtime_status';
        decode_failures_delta=$last.Controller.status.stream.gui_decode_failures-$first.Controller.status.stream.gui_decode_failures;
        present_failures_delta=$last.Controller.status.stream.gui_present_failures-$first.Controller.status.stream.gui_present_failures;
        input_batches_delta=$last.Controller.result.remote_input.sent_batches-$first.Controller.result.remote_input.sent_batches;
        input_control_lost=@($samples | Where-Object {-not $_.Controller.result.remote_input.control_enabled}).Count -gt 0;
        control_queue_delta=$last.Controller.result.runtime_stdio.control_messages-$first.Controller.result.runtime_stdio.control_messages;
        diagnostic_queue_delta=$last.Controller.result.runtime_stdio.diagnostic_pending_bytes-$first.Controller.result.runtime_stdio.diagnostic_pending_bytes;
        diagnostic_queue_peak_observed=($samples.Controller.result.runtime_stdio.diagnostic_pending_bytes | Measure-Object -Maximum).Maximum;
        agent_request_queue_delta=$last.Controller.status.agent.request_queue_depth-$first.Controller.status.agent.request_queue_depth;
        agent_result_queue_delta=$last.Controller.status.agent.result_queue_depth-$first.Controller.status.agent.result_queue_depth;
        task_windows=@{Host=$last.Host.result.task_timing;Controller=$last.Controller.result.task_timing};
        window_boundary=$(if($UntilComplete){'before_approval_to_gui_terminal_observation'}else{'gui_snapshot_to_gui_snapshot'});
        stages=$stages;first=$first;last=$last}
    $result | ConvertTo-Json -Depth 64 | Set-Content -LiteralPath (Join-Path $report ($Tag+'-window.json')) -Encoding UTF8
    if ($UntilComplete -and -not $completed) { throw 'Fixture did not complete; retain failed window.' }
    if ($UntilComplete) {
        foreach ($role in 'Host','Controller') {
            $task = $last[$role].result.task_timing
            $terminalReady = if ($HiddenPanel -and $role -eq 'Controller') {
                -not $task.panel_visible -and $task.terminal_consumed_us -gt 0
            } else { $task.terminal_painted_us -gt 0 }
            if ($task.retained_reply_utf16_units -ne 1048576 -or $task.output_gap -or -not $terminalReady) {
                throw ($role+': incomplete text or terminal not painted; retain failed window.')
            }
        }
    }
    Write-Host ("[gui-pressure] {0}: decode={1:N2} present={2:N2} duration={3:N2}s" -f $Tag,$result.decode_fps,$result.present_fps,$duration)
    return $result
}
function Export-PressureBoundaries($MeasuredWindows) {
    # Provider timestamps and GUI timestamps share this machine's steady clock.
    # Never infer output start from file modification time or approval time.
    $producerStarts = @{}
    foreach ($role in 'host','controller') {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $RunDirectory $role) -Filter "redclaw-desktop-$role-*.log") {
            foreach ($line in Get-Content -LiteralPath $file.FullName) {
                if ($line -match 'Agent fixture timing task_id=(\S+) first_output_us=(\d+) bytes=1048576') {
                    $producerStarts[$Matches[1]] = [long]$Matches[2]
                }
            }
        }
    }
    $boundaries = foreach ($window in $MeasuredWindows) {
        $pressure = $window.window -like '*-pressure'
        $begins=@{}; $ends=@{}
        foreach($role in 'Host','Controller') {
            $begins[$role]=if($pressure){$producerStarts[$window.last[$role].result.task_id]}else{$window.first[$role].status.gui_scheduling.sampled_at_us}
            $ends[$role]=if($pressure){
                if($HiddenPanel -and $role -eq 'Controller'){$window.task_windows[$role].terminal_consumed_us}
                else{$window.task_windows[$role].terminal_painted_us}
            }else{$window.last[$role].status.gui_scheduling.sampled_at_us}
        }
        # Both endpoints use the same complete source interval for round gates.
        # Idle uses the shared overlap, while pressure includes both producers.
        $roundBegin=if($pressure){($begins.Values|Measure-Object -Minimum).Minimum}else{($begins.Values|Measure-Object -Maximum).Maximum}
        $roundEnd=if($pressure){($ends.Values|Measure-Object -Maximum).Maximum}else{($ends.Values|Measure-Object -Minimum).Minimum}
        $roundWindows=@{}
        $tasks = @{}
        foreach ($role in 'Host','Controller') {
            $task = $window.task_windows[$role]
            $id = $window.last[$role].result.task_id
            $start = $producerStarts[$id]
            $beginUs = $begins[$role]
            $endUs = $ends[$role]
            $sourceWindowPath = Join-Path $report ($window.window+'-'+$role+'-source-window.json')
            if ($beginUs -gt 0 -and $endUs -gt $beginUs) {
                & (Join-Path $PSScriptRoot 'measure-gui-trace.ps1') -TracePath $window.traces[$role].trace_path `
                    -MetadataPath $window.traces[$role].metadata_path -StartUs $beginUs -EndUs $endUs -OutputPath $sourceWindowPath | Out-Host
                $roundPath=Join-Path $report ($window.window+'-'+$role+'-round-window.json')
                & (Join-Path $PSScriptRoot 'measure-gui-trace.ps1') -TracePath $window.traces[$role].trace_path `
                    -MetadataPath $window.traces[$role].metadata_path -StartUs $roundBegin -EndUs $roundEnd -OutputPath $roundPath | Out-Host
                $roundWindows[$role]=Get-Content -LiteralPath $roundPath -Raw|ConvertFrom-Json
                if($pressure -and $task.first_output_consumed_us -gt 0) {
                    & (Join-Path $PSScriptRoot 'measure-gui-trace.ps1') -TracePath $window.traces[$role].trace_path `
                        -MetadataPath $window.traces[$role].metadata_path -StartUs $task.first_output_consumed_us -EndUs $endUs `
                        -OutputPath (Join-Path $report ($window.window+'-'+$role+'-gui-consume-window.json')) | Out-Host
                }
                if($RealInputs -and $role -eq 'Controller' -and $window.input_evidence) {
                    $generation=(Get-Content -LiteralPath (Join-Path $report 'host-injection-export.json') -Raw|ConvertFrom-Json).result.generation
                    $injectionPath=Join-Path (Join-Path $RunDirectory 'host') ('input-injections-'+$generation+'.json')
                    & (Join-Path $PSScriptRoot 'measure-qa-input-receipts.ps1') -ReceiptPath $window.input_evidence.result.receipt_path `
                        -InjectionPath $injectionPath -StartUs $roundBegin -EndUs $roundEnd -OutputPath (Join-Path $report ($window.window+'-input-window.json')) | Out-Host
                }
            }
            $sourceWindow = if(Test-Path -LiteralPath $sourceWindowPath) {Get-Content -Raw -LiteralPath $sourceWindowPath | ConvertFrom-Json} else {$null}
            $tasks[$role] = @{task_id=$id;producer_first_output_us=$start;source_window=$sourceWindow;
                terminal_painted_us=$task.terminal_painted_us;
                source_to_visible_ms=$(if($start){($task.terminal_painted_us-$start)/1000}else{$null});
                complete_text=($task.retained_reply_utf16_units -eq 1048576 -and -not $task.output_gap);
                gui_window=$task.gui_window}
        }
        [pscustomobject]@{window=$window.window;tasks=$tasks;round_start_us=$roundBegin;round_end_us=$roundEnd;round_windows=$roundWindows;hidden_panel=[bool]$HiddenPanel;
            all_producer_boundaries_found=($tasks.Host.producer_first_output_us -gt 0 -and $tasks.Controller.producer_first_output_us -gt 0);
            stage_boundary=$(if($pressure){'provider_first_output_to_terminal_paint'}else{'gui_snapshot_to_gui_snapshot'});
            heartbeat_gate_passed=($roundWindows.Controller.heartbeat_gate_passed -and $roundWindows.Host.heartbeat_gate_passed)}
    }
    @($boundaries) | ConvertTo-Json -Depth 64 | Set-Content -LiteralPath (Join-Path $report 'pressure-boundaries.json') -Encoding UTF8
}
$ready = Snapshot 'ready'
if($HiddenPanel -and $ready.Controller.result.task_timing.panel_visible){throw 'Hide the owned Controller Agent panel before this run.'}
foreach ($role in 'Host','Controller') {
    if (-not ($ready[$role].result.providers | Where-Object {$_.version -eq 'debug-fixture-1' -and $_.available})) {
        throw 'Explicit no-command Debug fixture must be ready on both endpoints.'
    }
}
$windows = [Collections.Generic.List[object]]::new()
try {
    [void](Invoke-Qa 'Controller' 'remote_control_start')
    Start-Sleep -Milliseconds 500
    for ($round = 1; $round -le $Repetitions; ++$round) {
        $windows.Add((Measure-Window "$round-idle" $IdleSeconds))
        foreach ($role in 'Host','Controller') { [void](Invoke-Qa $role 'agent_start_fixture') }
        $clock = [Diagnostics.Stopwatch]::StartNew()
        do {
            $pending = Snapshot ("$round-pending-"+$clock.ElapsedMilliseconds)
            if ($pending.Host.result.approval_pending -and $pending.Controller.result.approval_pending) { break }
            Start-Sleep -Milliseconds 500
        } while ($clock.Elapsed.TotalSeconds -lt 30)
        if (-not $pending.Host.result.approval_pending -or -not $pending.Controller.result.approval_pending) { throw 'Approval prerequisite failed.' }
        # Persist boundaries before approval so output cannot precede the record.
        Arm-Trace "$round-pressure"
        $beforeApproval = Snapshot "$round-approved-before"
        foreach ($role in 'Host','Controller') { [void](Invoke-Qa $role 'agent_approval') }
        $windows.Add((Measure-Window "$round-pressure" 90 $true $beforeApproval))
        $windows.Add((Measure-Window "$round-recovered" $IdleSeconds))
    }
} finally {
    if ($RealInputs) { [void](Invoke-Qa 'Controller' 'input_probe_stop') }
    $windows | ConvertTo-Json -Depth 64 | Set-Content -LiteralPath (Join-Path $report 'windows.json') -Encoding UTF8
    $final = Snapshot 'cleanup-before'
    if ($final.Controller.result.remote_input.control_enabled) { [void](Invoke-Qa 'Controller' 'remote_control_pause') }
    if ($RealInputs) {
        $injectionExport=Invoke-Qa 'Host' 'input_probe_export'
        $injectionExport | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath (Join-Path $report 'host-injection-export.json') -Encoding UTF8
        $injectionPath=Join-Path (Join-Path $RunDirectory 'host') ('input-injections-'+$injectionExport.result.generation+'.json')
        $exportWait=[Diagnostics.Stopwatch]::StartNew()
        while(-not (Test-Path -LiteralPath $injectionPath) -and $exportWait.Elapsed.TotalSeconds -lt 10){Start-Sleep -Milliseconds 100}
        if(-not (Test-Path -LiteralPath $injectionPath)){throw 'Host injection export was accepted but did not finish.'}
    }
    Export-PressureBoundaries $windows
}
