param([Parameter(Mandatory=$true)][string]$RunDirectory,[int]$Seconds=30)
$ErrorActionPreference='Stop'
$run=Get-Content (Join-Path $RunDirectory result.json) -Raw|ConvertFrom-Json
$qa=Join-Path $PSScriptRoot 'invoke-cross-lan-debug-control.ps1'
$records=[Collections.Generic.List[object]]::new()
function Invoke-TraceQa([string]$Role,[string]$Action) {
    $r=& $qa -ControlName ('RedClawDesktop.LocalGui.'+$Role+'.'+$run.run_id) -Action $Action -Json|ConvertFrom-Json
    if(-not $r.ok){throw 'Trace overhead QA action failed.'}
    return $r
}
function Sample-TraceCost {
    $pair=@{}
    foreach($role in 'Host','Controller') {
        $s=Invoke-TraceQa $role 'agent_status'
        $process=Get-Process -Id $s.status.app_pid
        $pair[$role]=@{status=$s;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;
            private_bytes=$process.PrivateMemorySize64;working_set_bytes=$process.WorkingSet64}
    }
    return $pair
}
foreach($mode in 'off-before','on','off-after') {
    foreach($role in 'Host','Controller') {
        [void](Invoke-TraceQa $role $(if($mode -eq 'on'){'measurement_arm'}else{'measurement_export'}))
    }
    $first=Sample-TraceCost
    Start-Sleep -Seconds $Seconds
    $last=Sample-TraceCost
    $pair=@{}
    foreach($role in 'Host','Controller') {
        $before=$first[$role];$after=$last[$role]
        $duration=($after.status.status.gui_scheduling.sampled_at_us-$before.status.status.gui_scheduling.sampled_at_us)/1000000.0
        $pair[$role]=@{seconds=$duration;
            new_frame_fps=($after.status.status.gui_scheduling.totals.displayed_frame.count-$before.status.status.gui_scheduling.totals.displayed_frame.count)/$duration;
            cpu_core_percent=($after.cpu_ms-$before.cpu_ms)/($duration*10);
            private_bytes_delta=$after.private_bytes-$before.private_bytes;
            working_set_bytes_delta=$after.working_set_bytes-$before.working_set_bytes}
    }
    $records.Add(@{mode=$mode;roles=$pair;first=$first;last=$last})
}
@{schema='redclaw.gui-trace-overhead.v1';scope='QA detailed event recording; bounded lifetime timing remains enabled in all phases';
    records=@($records)}|ConvertTo-Json -Depth 64|Set-Content (Join-Path $RunDirectory 'trace-overhead.json')
$records|ForEach-Object {[pscustomobject]@{mode=$_.mode;fps=$_.roles.Controller.new_frame_fps;gui_cpu_core_percent=$_.roles.Controller.cpu_core_percent}}
