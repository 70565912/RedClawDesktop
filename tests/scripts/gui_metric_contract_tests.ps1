param([string]$OutputRoot = (Join-Path $PSScriptRoot '../../build/reports/metric-contract-tests'))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$root=Join-Path $OutputRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
function Check($Value,[string]$Message){if(-not $Value){throw $Message}}
function Write-Json($Value,[string]$Path){ConvertTo-Json -InputObject $Value -Depth 32 | Set-Content -LiteralPath $Path -Encoding UTF8}
function Case([string]$Name,[long]$Receive,[long]$Batch,[long]$Other,[bool]$InputPass,[bool]$AckPass,[bool]$Contract=$true,[bool]$MissingAck=$false,[long]$LateAck=0,[long]$EmptySync=20001,[long]$StateSync=0){
    $dir=Join-Path $root $Name; New-Item -ItemType Directory -Path $dir | Out-Null
    $windows=@();$boundaries=@()
    foreach($round in 1..3){foreach($phase in 'idle','pressure','recovered'){
        $tag="$round-$phase"
        $stages=@{input_ack_local_delivery=@{count=2;max_us=$Batch};input_batch_local_delivery=@{count=1;max_us=$Batch};control_status_local_delivery=@{count=1;max_us=$Other}}
        if($EmptySync){$stages.input_sync_local_delivery=@{count=1;max_us=$EmptySync}}
        if($StateSync){$stages.input_state_sync_local_delivery=@{count=1;max_us=$StateSync}}
        # The old all-status stage makes the pre-fix regression see this delay.
        if($Other -gt $Batch){$stages.input_ack_local_delivery.max_us=$Other}
        $trace=@{valid=$true;queue_evidence_available=$true;present_fps=25;stages=$stages;metric_contract=$(if($Contract){'redclaw.gui-timing.v2'}else{'unknown'});invalid_ack_timings=0}
        $boundaries+=@{window=$tag;round_start_us=1000000;round_end_us=31000000;heartbeat_gate_passed=$true;round_windows=@{Host=$trace;Controller=$trace};tasks=@{Host=@{complete_text=$true};Controller=@{complete_text=$true}}}
        $status=@{diagnostic_writer=@{evidence_integrity=$true};channel_open=$true;agent=@{channel_open=$true};stream=@{gui_decode_failures=0;gui_present_failures=0}}
        $windows+=@{window=$tag;input_control_lost=$false;last=@{Host=@{status=$status};Controller=@{status=$status;result=@{remote_input=@{control_enabled=$true}}}}}
        $receipts=@(foreach($i in 1..100){foreach($kind in 0,2){@{sequence=$i;kind=$kind;receive_latency_us=$Receive;ack_local_us=$Batch}}})
        Write-Json @{valid=$true;receipts=$receipts;ack_valid=(-not $MissingAck);ack_precision='microseconds';
            ack_local=@{count=200;max_us=[math]::Max($Batch,$LateAck)}} (Join-Path $dir ($tag+'-input-window.json'))
    }}
    Write-Json $windows (Join-Path $dir 'windows.json'); Write-Json $boundaries (Join-Path $dir 'pressure-boundaries.json')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'scripts/service/evaluate-gui-local-gates.ps1') -ReportDirectory $dir *> (Join-Path $dir 'evaluation.log')
    $exit=$LASTEXITCODE
    $result=Get-Content -LiteralPath (Join-Path $dir 'configuration-gates.json') -Raw|ConvertFrom-Json
    Check ($result.gates.input_pressure -eq $InputPass) "$Name input result must not depend on ACK"
    Check ($result.gates.ack_local_delivery -eq $AckPass) "$Name ACK classification/threshold incorrect"
    Check ($result.passed -eq ($InputPass -and $AckPass)) "$Name combined result incorrect"
    Check ($exit -eq $(if($InputPass -and $AckPass){0}else{1})) "$Name exit mismatch"
}
Case 'slow-ack-fast-input' 80001 300001 0 $true $false
Case 'slow-input-fast-ack' 100001 20001 0 $false $true
Case 'other-status-is-not-input-ack' 80001 20001 400001 $true $true
Case 'ack-exactly-250ms-fails' 80001 250000 0 $true $false
Case 'input-exactly-100ms-passes' 100000 249999 0 $true $true
Case 'old-contract-is-unverified' 80001 20001 0 $true $false $false
Case 'missing-ACK-does-not-fail-input' 80001 20001 0 $true $false $true $true
Case 'late-ACK-outside-trace-window-fails' 80001 20001 0 $true $false $true $false 300001
Case -Name 'empty-sync-delay-fails-ACK-only' -Receive 80001 -Batch 20001 -InputPass $true -AckPass $false -EmptySync 250000
Case -Name 'held-state-sync-delay-fails-ACK-only' -Receive 80001 -Batch 20001 -InputPass $true -AckPass $false -StateSync 250000
Case -Name 'absent-sync-class-is-no-samples' -Receive 80001 -Batch 20001 -InputPass $true -AckPass $true -EmptySync 0

# An actual receipt arriving beyond the source boundary remains in the input
# distribution, even when its ACK is missing. This exercises the raw formatter.
$receiptPath=Join-Path $root 'native.json';$injectionPath=Join-Path $root 'injections.json'
Write-Json @{ack_precision='microseconds';overflow=0;interfered=$false;error='';receipts=@(
    @{sequence=7;kind=0;sent_us=1900000;received_us=2000001;runtime_ack_us=0;gui_ack_us=0},
    @{sequence=8;kind=2;sent_us=1900001;received_us=1900101;runtime_ack_us=1900201;gui_ack_us=1900238})} $receiptPath
Write-Json @{overflow=0;receipts=@(
    @{sequence=7;type=0;begin_us=1900050;end_us=1900090;injected=$true},
    @{sequence=8;type=3;begin_us=1900051;end_us=1900091;injected=$true},
    @{sequence=99;type=0;begin_us=2900050;end_us=2900090;injected=$true},
    @{sequence=99;type=0;begin_us=2900050;end_us=2900090;injected=$true})} $injectionPath
$outputPath=Join-Path $root 'native-window.json'
& (Join-Path $repo 'scripts/service/measure-qa-input-receipts.ps1') -ReceiptPath $receiptPath -InjectionPath $injectionPath -StartUs 1000000 -EndUs 2000000 -OutputPath $outputPath | Out-Null
$receipt=Get-Content -LiteralPath $outputPath -Raw|ConvertFrom-Json
Check ($receipt.valid -and -not $receipt.ack_valid -and $receipt.invalid_ack_receipts -eq 1) 'Missing ACK must not invalidate native receipt evidence'
Check ($receipt.keyboard.p95_us -eq 100001 -and -not $receipt.keyboard_latency_passed -and $receipt.receipts[0].crosses_end) 'Late receipt must retain the failed input latency'
Check ($receipt.receipts[1].ack_local_us -eq 37) 'Raw ACK microseconds must be retained'
Check ($receipt.receipts.Count -eq 2) 'Duplicate injection outside the selected window must not invalidate it'

# Startup is a separate measured interval; a later running stall still fails.
$tracePath=Join-Path $root 'startup.bin';$metadataPath=Join-Path $root 'metadata.json'
$stream=[IO.BinaryWriter]::new([IO.File]::Create($tracePath))
try {
    $stream.Write([Text.Encoding]::ASCII.GetBytes('RCDTRC01'));$stream.Write([uint32]4);$stream.Write([uint64]0);$stream.Write([uint64]1000000)
    foreach($sample in @(@(0,1000000,4000000),@(1,4000000,4010000),@(1,4010000,4310000),@(2,4500000,4500000))){
        $stream.Write([uint32]$sample[0]);$stream.Write([uint64]$sample[1]);$stream.Write([uint64]$sample[2]);$stream.Write([uint64]0)
    }
} finally {$stream.Dispose()}
Write-Json @{result=@{measurement=@{metric_contract='redclaw.gui-timing.v2';invalid_ack_timings=0;stage_names=@('startup_first_heartbeat','heartbeat_gap','displayed_frame')}}} $metadataPath
foreach($scope in @(@('early',4010001,$true),@('later',4400000,$false))){
    $outputPath=Join-Path $root ($scope[0]+'-trace.json')
    & (Join-Path $repo 'scripts/service/measure-gui-trace.ps1') -TracePath $tracePath -MetadataPath $metadataPath -StartUs 1000000 -EndUs $scope[1] -OutputPath $outputPath | Out-Null
    $trace=Get-Content -LiteralPath $outputPath -Raw|ConvertFrom-Json
    Check ($trace.valid -and $trace.heartbeat_gate_passed -eq $scope[2]) 'Startup and running heartbeat must be independent'
    Check ($trace.stages.startup_first_heartbeat.max_us -eq 3000000 -and $null -eq $trace.stages.startup_first_heartbeat.cpu_max_us) 'Startup delay and unavailable CPU must remain explicit'
}
Write-Output "PASS: eleven independent input/ACK gate cases, raw receipts and startup windows ($root)"
