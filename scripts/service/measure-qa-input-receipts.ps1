param(
    [Parameter(Mandatory=$true)][string]$ReceiptPath,
    [Parameter(Mandatory=$true)][string]$InjectionPath,
    [Parameter(Mandatory=$true)][long]$StartUs,
    [Parameter(Mandatory=$true)][long]$EndUs,
    [Parameter(Mandatory=$true)][string]$OutputPath
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputPath){throw 'Preserve existing receipt evidence; choose a new OutputPath.'}
$target=Get-Content -LiteralPath $ReceiptPath -Raw|ConvertFrom-Json
$hostReceipts=Get-Content -LiteralPath $InjectionPath -Raw|ConvertFrom-Json
$selectedReceipts=@($target.receipts|Where-Object {$_.sent_us -ge $StartUs -and $_.sent_us -lt $EndUs})
$selectedKeys=@{}
foreach($r in $selectedReceipts){$selectedKeys[([string]$r.sequence)+':'+$r.kind]=$true}
$injections=@{}
foreach($r in $hostReceipts.receipts){
    if($r.type -eq 2){continue}
    $kind=switch([int]$r.type){0{0}1{1}3{2}4{3}default{-1}}
    if($kind -lt 0){continue}
    $key=([string]$r.sequence)+':'+$kind
    # A runtime export covers the complete QA session. An unrelated invalid
    # interval must not prevent measuring this send-attributed window.
    if(-not $selectedKeys.ContainsKey($key)){continue}
    if($injections.ContainsKey($key)){throw 'Duplicate injection sequence/type; cannot correlate unambiguously.'}
    $injections[$key]=$r
}
$rows=@(foreach($r in $selectedReceipts){
    # Arrivals after the source window are retained as crossing receipts, never
    # silently excluded from latency.
    $i=$injections[([string]$r.sequence)+':'+$r.kind]
    [pscustomobject]@{sequence=$r.sequence;kind=$r.kind;sent_us=$r.sent_us;
        host_begin_us=$i.begin_us;host_end_us=$i.end_us;injected=$i.injected;
        received_us=$r.received_us;runtime_ack_us=$r.runtime_ack_us;gui_ack_us=$r.gui_ack_us;
        receive_latency_us=$(if($r.received_us -ge $r.sent_us){$r.received_us-$r.sent_us}else{$null});
        ack_local_us=$(if($r.gui_ack_us -ge $r.runtime_ack_us -and $r.runtime_ack_us -gt 0){$r.gui_ack_us-$r.runtime_ack_us}else{$null});
        crosses_end=($r.received_us -ge $EndUs)}
})
function Distribution($Values){
    $v=@($Values|Where-Object {$null -ne $_}|Sort-Object);$n=$v.Count
    if(-not $n){return @{count=0}}
    return @{count=$n;p50_us=$v[[math]::Ceiling($n*.5)-1];p95_us=$v[[math]::Ceiling($n*.95)-1];p99_us=$v[[math]::Ceiling($n*.99)-1];max_us=$v[-1]}
}
$keyboard=Distribution @($rows|Where-Object kind -LT 2|ForEach-Object receive_latency_us)
$mouse=Distribution @($rows|Where-Object kind -GE 2|ForEach-Object receive_latency_us)
$ack=Distribution @($rows.ack_local_us)
$invalid=@($rows|Where-Object {-not $_.injected -or -not $_.received_us -or $_.received_us -lt $_.sent_us -or $_.host_begin_us -lt $_.sent_us -or $_.host_end_us -lt $_.host_begin_us}).Count
$invalidAck=@($rows|Where-Object {-not $_.runtime_ack_us -or $_.runtime_ack_us -lt $_.sent_us -or $_.gui_ack_us -lt $_.runtime_ack_us}).Count
$valid=-not $target.interfered -and -not $target.error -and $target.overflow -eq 0 -and $hostReceipts.overflow -eq 0 -and $invalid -eq 0 -and $EndUs -gt $StartUs
$ackValid=$valid -and $invalidAck -eq 0 -and $target.ack_precision -eq 'microseconds'
$result=[ordered]@{schema='redclaw.qa-input-window.v2';start_us=$StartUs;end_us=$EndUs;valid=$valid;
    receive_scope='actual_send_to_native_dispatch_key_and_left_button';sample_attribution='sent_in_window_including_late_receipts';
    ack_valid=$ackValid;invalid_ack_receipts=$invalidAck;ack_precision=$(if($target.ack_precision){$target.ack_precision}else{'unverified'});
    interference=$target.interfered;error=$target.error;invalid_receipts=$invalid;keyboard=$keyboard;mouse=$mouse;ack_local=$ack;
    keyboard_latency_passed=($valid -and $keyboard.count -gt 0 -and $keyboard.p95_us -le 100000);
    mouse_latency_passed=($valid -and $mouse.count -gt 0 -and $mouse.p95_us -le 100000);
    ack_local_passed=($ackValid -and $ack.count -gt 0 -and $ack.max_us -lt 250000);
    unique_keyboard_batches=@($rows|Where-Object kind -LT 2|ForEach-Object sequence|Sort-Object -Unique).Count;
    unique_mouse_batches=@($rows|Where-Object kind -GE 2|ForEach-Object sequence|Sort-Object -Unique).Count;
    target_pid=$target.target_pid;keyboard_target_hwnd=$target.keyboard_target_hwnd;mouse_target_hwnd=$target.mouse_target_hwnd;
    geometry_revision=$target.geometry_revision;receipt_source=$ReceiptPath;injection_source=$InjectionPath;receipts=$rows}
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $OutputPath -Encoding UTF8
[pscustomobject]@{valid=$valid;keyboard_count=$keyboard.count;keyboard_p95_ms=$keyboard.p95_us/1000;mouse_count=$mouse.count;mouse_p95_ms=$mouse.p95_us/1000;ack_max_ms=$ack.max_us/1000}
