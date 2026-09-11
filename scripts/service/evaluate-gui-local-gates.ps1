param(
    [Parameter(Mandatory=$true)][string]$ReportDirectory,
    [switch]$NativeSize,
    [string]$OutputPath
)
$ErrorActionPreference='Stop'
if(-not $OutputPath){$OutputPath=Join-Path $ReportDirectory 'configuration-gates.json'}
if(Test-Path -LiteralPath $OutputPath){throw 'Preserve existing gate evidence; choose a new OutputPath.'}
$windows=Get-Content -LiteralPath (Join-Path $ReportDirectory 'windows.json') -Raw|ConvertFrom-Json
$boundaries=Get-Content -LiteralPath (Join-Path $ReportDirectory 'pressure-boundaries.json') -Raw|ConvertFrom-Json
$rounds=@(foreach($idle in $boundaries|Where-Object window -Like '*-idle') {
    $id=$idle.window.Split('-')[0]
    $pressure=$boundaries|Where-Object window -EQ ($id+'-pressure')
    $recovery=$boundaries|Where-Object window -EQ ($id+'-recovered')
    $fps=[double]$idle.round_windows.Controller.present_fps
    $pressureRatio=if($fps -gt 0){$pressure.round_windows.Controller.present_fps/$fps}else{0}
    $recoveryRatio=if($fps -gt 0){$recovery.round_windows.Controller.present_fps/$fps}else{0}
    $heartbeatClassified=$true
    foreach($window in @($idle,$pressure,$recovery)){foreach($role in 'Host','Controller'){
        $heartbeatClassified=$heartbeatClassified -and $window.round_windows.$role.metric_contract -eq 'redclaw.gui-timing.v2'
    }}
    [pscustomobject]@{round=[int]$id;idle_fps=$fps;pressure_fps=$pressure.round_windows.Controller.present_fps;
        recovery_fps=$recovery.round_windows.Controller.present_fps;pressure_ratio=$pressureRatio;recovery_ratio=$recoveryRatio;
        fps_passed=($fps -gt 0 -and $pressureRatio -ge .90 -and $recoveryRatio -ge .90);
        heartbeat_measurement_status=$(if($heartbeatClassified){'classified'}else{'unverified'});
        heartbeat_passed=($heartbeatClassified -and $idle.heartbeat_gate_passed -and $pressure.heartbeat_gate_passed -and $recovery.heartbeat_gate_passed);
        text_passed=($pressure.tasks.Host.complete_text -and $pressure.tasks.Controller.complete_text)}
})
$traceValid=$boundaries.Count -eq $windows.Count -and $windows.Count -ge 9
$ackLocalValid=$true; $ackLocalMaximum=$null; $ackWindows=@()
foreach($b in $boundaries){foreach($role in 'Host','Controller'){
    $traceValid=$traceValid -and $b.round_windows.$role.valid -and $b.round_windows.$role.queue_evidence_available
}
    $trace=$b.round_windows.Controller
    $classified=$trace.metric_contract -eq 'redclaw.gui-timing.v2' -and $trace.invalid_ack_timings -eq 0
    $groups=@{}; $observedCount=0L
    foreach($entry in @(@('batch','input_batch_local_delivery'),@('empty_sync','input_sync_local_delivery'),@('state_sync','input_state_sync_local_delivery'))){
        $sample=$trace.stages.($entry[1]); $count=[long]$sample.count
        $observedCount+=$count
        $ok=$classified -and ($count -eq 0 -or $sample.max_us -lt 250000)
        $groups[$entry[0]]=@{count=$count;max_us=$(if($classified -and $count){[long]$sample.max_us}else{$null});
            statistics=$sample;status=$(if(-not $classified){'unverified'}elseif(-not $count){'no_samples'}elseif($ok){'passed'}else{'failed'});passed=$(if($count){$ok}else{$null})}
        $ackLocalValid=$ackLocalValid -and $ok
        if($classified -and $count){$ackLocalMaximum=[math]::Max([long]$ackLocalMaximum,[long]$sample.max_us)}
    }
    $ackLocalValid=$ackLocalValid -and $observedCount -gt 0
    # Receipts retain ACKs arriving after a window closes. Missing ACKs cannot
    # disappear simply because other, completed ACKs have a good distribution.
    $receiptPath=Join-Path $ReportDirectory ($b.window+'-input-window.json')
    $receiptAck=$null
    if(Test-Path -LiteralPath $receiptPath){$receiptAck=Get-Content -LiteralPath $receiptPath -Raw|ConvertFrom-Json}
    $receiptVerified=$null -ne $receiptAck -and $receiptAck.ack_valid -and $receiptAck.ack_precision -eq 'microseconds'
    $receiptPassed=$receiptVerified -and $receiptAck.ack_local.count -gt 0 -and $receiptAck.ack_local.max_us -lt 250000
    $ackLocalValid=$ackLocalValid -and $receiptPassed
    if($receiptVerified -and $receiptAck.ack_local.count){$ackLocalMaximum=[math]::Max([long]$ackLocalMaximum,[long]$receiptAck.ack_local.max_us)}
    $ackWindows+=@{window=$b.window;classified=$classified;classes=$groups;
        receipt_ack=@{source=$receiptPath;verified=$receiptVerified;passed=$receiptPassed;
            invalid_receipts=$receiptAck.invalid_ack_receipts;statistics=$receiptAck.ack_local;
            sample_unit='native event; correlated down/up share an ACK; trace classes count sequences';
            attribution='sent_in_window_including_late_ACKs'};
        other_control_status=$trace.stages.control_status_local_delivery}
}
$diagnosticsValid=$true; $decodePresentValid=$true; $channelsValid=$true; $leaseValid=$true; $nativeValid=$true
foreach($w in $windows){foreach($role in 'Host','Controller'){
    $s=$w.last.$role.status
    $diagnosticsValid=$diagnosticsValid -and $s.diagnostic_writer.evidence_integrity
    $channelsValid=$channelsValid -and $s.channel_open -and $s.agent.channel_open
    $decodePresentValid=$decodePresentValid -and $s.stream.gui_decode_failures -eq 0 -and $s.stream.gui_present_failures -eq 0
}
    $leaseValid=$leaseValid -and -not $w.input_control_lost -and $w.last.Controller.result.remote_input.control_enabled
    if($NativeSize){
        $p=$w.last.Controller.result.playback
        $nativeValid=$nativeValid -and $p.last_presented_surface -and $p.decoded_surface_frames -gt 0 -and
            $p.cpu_transfer_frames -eq 0 -and $p.surface_fallbacks -eq 0 -and $p.software_frames -eq 0 -and
            $p.encoded_visible_width -eq $p.content_width -and $p.encoded_visible_height -eq $p.content_height -and
            $p.content_width -eq $w.input_evidence.result.input_probe.capture_width -and
            $p.content_height -eq $w.input_evidence.result.input_probe.capture_height
    }
}
function Distribution($Values){
    $v=@($Values|Where-Object {$null -ne $_}|Sort-Object);$n=$v.Count
    if(-not $n){return @{count=0}}
    return @{count=$n;p50_us=$v[[math]::Ceiling($n*.5)-1];p95_us=$v[[math]::Ceiling($n*.95)-1];p99_us=$v[[math]::Ceiling($n*.99)-1];max_us=$v[-1]}
}
$inputs=@{}
foreach($phase in 'idle','pressure'){
    $receipts=[Collections.Generic.List[object]]::new();$valid=$true;$sources=@()
    foreach($w in $windows|Where-Object window -Like ('*-'+$phase)){
        $path=Join-Path $ReportDirectory ($w.window+'-input-window.json')
        if(-not (Test-Path -LiteralPath $path)){$valid=$false;continue}
        $r=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
        $valid=$valid -and $r.valid;$sources+=$path
        foreach($receipt in $r.receipts){$receipts.Add($receipt)}
    }
    $keyboard=Distribution @($receipts|Where-Object kind -LT 2|ForEach-Object receive_latency_us)
    $mouse=Distribution @($receipts|Where-Object kind -GE 2|ForEach-Object receive_latency_us)
    $inputs[$phase]=@{valid=$valid;keyboard=$keyboard;mouse=$mouse;sources=$sources;
        unique_keyboard_batches=@($receipts|Where-Object kind -LT 2|ForEach-Object sequence|Sort-Object -Unique).Count;
        unique_mouse_batches=@($receipts|Where-Object kind -GE 2|ForEach-Object sequence|Sort-Object -Unique).Count;
        passed=($valid -and $keyboard.count -ge 100 -and $mouse.count -ge 100 -and $keyboard.p95_us -le 100000 -and
            $mouse.p95_us -le 100000)}
}
$gates=[ordered]@{at_least_three_rounds=$rounds.Count -ge 3;complete_trace=$traceValid;diagnostic_integrity=$diagnosticsValid;
    idle_recovery_duration=@($boundaries|Where-Object {$_.window -notlike '*-pressure' -and ($_.round_end_us-$_.round_start_us) -lt 30000000}).Count -eq 0;
    heartbeat=@($rounds|Where-Object {-not $_.heartbeat_passed}).Count -eq 0;
    relative_fps=@($rounds|Where-Object {-not $_.fps_passed}).Count -eq 0;
    complete_text=@($rounds|Where-Object {-not $_.text_passed}).Count -eq 0;
    decode_present=$decodePresentValid;channels=$channelsValid;input_lease=$leaseValid;
    input_idle=$inputs.idle.passed;input_pressure=$inputs.pressure.passed;ack_local_delivery=$ackLocalValid;native_surface=$(if($NativeSize){$nativeValid}else{$null})}
$passed=@($gates.Values|Where-Object {$null -ne $_ -and -not $_}).Count -eq 0
$result=[ordered]@{schema='redclaw.local-performance-config-gates.v2';native_size=[bool]$NativeSize;passed=$passed;
    gates=$gates;rounds=$rounds;inputs=$inputs;ack_local_maximum_us=$ackLocalMaximum;
    acknowledgements=@{clock='monotonic_microseconds';windows=$ackWindows;quantiles='per_window_exact_trace; not pooled from percentiles';
        maximum_scope='verified samples only; incomplete evidence cannot pass';invalid_timing_scope='session cumulative at trace export'};
    input_scope='actual send to native target dispatch; pooled by configuration and phase; correlated down/up events';
    workload='Debug fixture: each direction 128 unpaced 8192-byte long-line chunks';
    scope='Measured idle/pressure/recovery only. Startup/first dispatch are separate from running heartbeat. Cold starts, geometry, visible panel restoration, process exits and settings restoration require separate evidence.'}
$result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $OutputPath -Encoding UTF8
$gates|ConvertTo-Json
if(-not $passed){exit 1}
