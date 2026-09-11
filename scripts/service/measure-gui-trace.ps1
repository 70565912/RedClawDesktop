param(
    [Parameter(Mandatory=$true)][string]$TracePath,
    [Parameter(Mandatory=$true)][string]$MetadataPath,
    [long]$StartUs = 0,
    [long]$EndUs = 0,
    [Parameter(Mandatory=$true)][string]$OutputPath
)
$ErrorActionPreference = 'Stop'
if(Test-Path -LiteralPath $OutputPath){throw 'Preserve existing trace-window evidence; choose a new OutputPath.'}
$metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
$names = @($metadata.result.measurement.stage_names)
if (-not $names.Count) { throw 'Trace export response with stage_names is required.' }
$file = [IO.File]::OpenRead((Resolve-Path -LiteralPath $TracePath))
$reader = [IO.BinaryReader]::new($file)
$samples = [Collections.Generic.List[object]]::new()
try {
    if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(8)) -ne 'RCDTRC01') { throw 'Invalid trace magic.' }
    $count = $reader.ReadUInt32(); $overflow = $reader.ReadUInt64(); $armedUs = $reader.ReadUInt64()
    if ($count -gt 262144 -or $file.Length -ne 28 + 28L*$count) { throw 'Invalid bounded trace length.' }
    for ($i=0; $i -lt $count; ++$i) {
        $stage=$reader.ReadUInt32(); $begin=$reader.ReadUInt64(); $end=$reader.ReadUInt64(); $cpu=$reader.ReadUInt64()
        if ($stage -ge $names.Count -or $end -lt $begin) { throw 'Invalid trace record.' }
        $samples.Add([pscustomobject]@{stage=$names[$stage];begin_us=[long]$begin;end_us=[long]$end;cpu_us=[long]$cpu})
    }
} finally { $reader.Dispose(); $file.Dispose() }
$traceEnd = ($samples | Measure-Object end_us -Maximum).Maximum
if (-not $StartUs) { $StartUs=[long]$armedUs }
if (-not $EndUs) { $EndUs=[long]$traceEnd }
$valid = $overflow -eq 0 -and $StartUs -ge $armedUs -and $EndUs -le $traceEnd -and $EndUs -gt $StartUs
$stages = [ordered]@{}
foreach ($group in ($samples | Where-Object {$_.end_us -ge $StartUs -and $_.end_us -lt $EndUs} | Group-Object stage)) {
    $durations = @($group.Group | ForEach-Object {$_.end_us-$_.begin_us} | Sort-Object)
    $n=$durations.Count
    $cpu = @($group.Group.cpu_us | Sort-Object)
    $nonExecution = @($group.Group | ForEach-Object {[math]::Max(0,$_.end_us-$_.begin_us-$_.cpu_us)} | Sort-Object)
    $stages[$group.Name] = [ordered]@{
        count=$n;total_us=($durations|Measure-Object -Sum).Sum;max_us=$durations[-1];
        p50_us=$durations[[math]::Max(0,[math]::Ceiling($n*0.50)-1)];
        p95_us=$durations[[math]::Max(0,[math]::Ceiling($n*0.95)-1)];
        p99_us=$durations[[math]::Max(0,[math]::Ceiling($n*0.99)-1)];
        cpu_total_us=($group.Group|Measure-Object cpu_us -Sum).Sum;
        cpu_max_us=$cpu[-1];cpu_p50_us=$cpu[[math]::Ceiling($n*.50)-1];cpu_p95_us=$cpu[[math]::Ceiling($n*.95)-1];cpu_p99_us=$cpu[[math]::Ceiling($n*.99)-1];
        non_execution_max_us=$nonExecution[-1];non_execution_p95_us=$nonExecution[[math]::Ceiling($n*.95)-1];
        crosses_start_count=@($group.Group|Where-Object begin_us -LT $StartUs).Count
    }
    # These intervals/counters do not sample thread CPU. Zero in their binary
    # record means unavailable, not proof that the entire interval was a wait.
    $cpuAvailable=$group.Name -notmatch '^(heartbeat_gap|dispatch_wait|event_wait|input_.*|startup_first_.*|control_status_local_delivery|displayed_frame|redraw|present_failure)$'
    $stages[$group.Name].cpu_available=$cpuAvailable
    if(-not $cpuAvailable){
        foreach($field in @($stages[$group.Name].Keys)|Where-Object {$_ -like 'cpu_*' -and $_ -ne 'cpu_available' -or $_ -like 'non_execution_*'}){
            $stages[$group.Name][$field]=$null
        }
    }
}
$classified=$metadata.result.measurement.metric_contract -eq 'redclaw.gui-timing.v2'
$result=[ordered]@{schema='redclaw.gui-trace-window.v2';valid=$valid;overflow=$overflow;
    metric_contract=$metadata.result.measurement.metric_contract;invalid_ack_timings=$metadata.result.measurement.invalid_ack_timings;
    heartbeat_definition=$(if($classified){'adjacent callbacks after event-loop entry; startup first dispatch/heartbeat are separate'}else{'legacy: first callback may include construction; no startup reclassification'});
    heartbeat_measurement_status=$(if($classified){'classified'}else{'unverified'});
    armed_us=$armedUs;trace_last_event_us=$traceEnd;start_us=$StartUs;end_us=$EndUs;
    seconds=($EndUs-$StartUs)/1000000.0;sample_attribution='completed_in_half_open_window';
    cpu_clock='process thread user plus kernel; Windows accounting granularity may exceed a short sample';
    non_execution_meaning='max(wall minus thread CPU,0); does not identify a wait cause';
    crosses_end_count=@($samples|Where-Object {$_.begin_us -lt $EndUs -and $_.end_us -ge $EndUs}).Count;
    stages=$stages;
    present_fps=$(if($EndUs -gt $StartUs){[long]$stages.displayed_frame.count/(($EndUs-$StartUs)/1000000.0)}else{0});
    heartbeat_gate_passed=($classified -and $valid -and $stages.heartbeat_gap.count -gt 0 -and $stages.heartbeat_gap.max_us -lt 250000)}
$queueNames=@('control_messages','control_bytes','diagnostic_receive_bytes','diagnostic_write_bytes')
$queues=[ordered]@{}
if(Test-Path -LiteralPath ($TracePath+'.queues.bin')) {
    $queueReader=[IO.BinaryReader]::new([IO.File]::OpenRead($TracePath+'.queues.bin'))
    try {
        if([Text.Encoding]::ASCII.GetString($queueReader.ReadBytes(8)) -ne 'RCDQUE01'){throw 'Invalid queue trace magic.'}
        $n=$queueReader.ReadUInt32();$lost=$queueReader.ReadUInt64();$queueStart=$queueReader.ReadUInt64()
        if($n -gt 262144 -or $queueReader.BaseStream.Length -ne 28+20L*$n -or $lost -gt 0 -or $queueStart -ne $armedUs){throw 'Invalid queue trace capacity, completeness or start identity.'}
        $records=@{}
        foreach($name in $queueNames){$records[$name]=[Collections.Generic.List[object]]::new()}
        for($i=0;$i -lt $n;++$i){$kind=$queueReader.ReadUInt32();$at=$queueReader.ReadUInt64();$value=$queueReader.ReadUInt64();if($kind -ge $queueNames.Count){throw 'Unknown queue kind.'};$records[$queueNames[$kind]].Add([pscustomobject]@{at=[long]$at;value=[long]$value})}
        foreach($name in $queueNames){
            $all=$records[$name];$initial=($all|Where-Object at -LE $StartUs|Select-Object -Last 1).value
            $inside=@($all|Where-Object {$_.at -gt $StartUs -and $_.at -lt $EndUs})
            $final=if($inside.Count){$inside[-1].value}else{$initial}
            $peak=(@($initial)+@($inside.value)|Measure-Object -Maximum).Maximum
            $queues[$name]=@{start=$initial;end=$final;peak=$peak;delta=$final-$initial;samples=$inside.Count}
        }
    } finally {$queueReader.Dispose()}
}
$result.queue_evidence_available=$queues.Count -eq $queueNames.Count
$result.queues=$queues
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $OutputPath -Encoding UTF8
[pscustomobject]@{valid=$valid;seconds=$result.seconds;present_fps=$result.present_fps;heartbeat_gate_passed=$result.heartbeat_gate_passed}
if(-not $valid){exit 1}
