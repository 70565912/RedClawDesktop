param(
    [Parameter(Mandatory)][string]$EvidenceDirectory,
    [Parameter(Mandatory)][string]$ProcDumpPath,
    [int]$GuiPid = 0,
    [string]$StatusPath = '',
    [string]$ExpectedExecutable = '',
    [string]$TaskName = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
Assert-UpgradeOwner $EvidenceDirectory
$evidence = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$statePath = Join-Path $evidence 'monitor-state.json'
$eventPath = Join-Path $evidence 'process-events.jsonl'
$targets = [Collections.Generic.List[object]]::new()
$phase = 'starting'
$lastSnapshot = $null
$failure = ''
$sequence = 0
$launchHost = $GuiPid -eq 0

function Read-LifecycleText {
    param([string]$Path)
    $stream = $null
    try {
        $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        if ($stream.Length -gt 2MB) { throw 'lifecycle_input_size_limit' }
        $buffer = New-Object byte[] $stream.Length
        $count = $stream.Read($buffer,0,$buffer.Length)
        if ($count -gt 1 -and ($buffer[1] -eq 0 -or $buffer[0] -eq 255)) { return [Text.Encoding]::Unicode.GetString($buffer,0,$count) }
        return [Text.Encoding]::UTF8.GetString($buffer,0,$count).TrimStart([char]0xFEFF)
    } catch [IO.IOException] { return '' }
    finally { if ($stream) { $stream.Dispose() } }
}

function Write-LifecycleEvent {
    param([string]$Kind, $Data)
    $entry = [ordered]@{schema='redclaw.process-lifecycle.event.v1'; time=[DateTimeOffset]::UtcNow.ToString('o'); kind=$Kind; data=$Data}
    if ((Test-Path -LiteralPath $eventPath) -and (Get-Item -LiteralPath $eventPath).Length -gt 8MB) {
        Move-Item -LiteralPath $eventPath -Destination ($eventPath + '.previous') -Force
    }
    [IO.File]::AppendAllText($eventPath, (($entry | ConvertTo-Json -Depth 6 -Compress) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
}

function Write-LifecycleState {
    $records = @($targets | ForEach-Object { $_.record })
    $collectorsReady = @($targets | Where-Object { -not $_.record.exited -and (-not $_.record.collector_ready -or $_.record.collector_exited) }).Count -eq 0
    $state = [ordered]@{
        schema='redclaw.process-lifecycle.state.v1'; updated_at=[DateTimeOffset]::UtcNow.ToString('o')
        monitor_pid=$PID; task_name=$TaskName; phase=$phase; status_path=$StatusPath
        collectors_ready=$collectorsReady; targets=$records; last_runtime_status=$lastSnapshot
        dump_count=@(Get-ChildItem -LiteralPath $evidence -Recurse -File -Filter '*.dmp').Count
        failure=$failure; automatic_restart=$false
    }
    $temporary = $statePath + '.tmp'
    [IO.File]::WriteAllText($temporary, ($state | ConvertTo-Json -Depth 7), [Text.UTF8Encoding]::new($false))
    for ($attempt=0; ; $attempt++) {
        try {
            if ([IO.File]::Exists($statePath)) { [IO.File]::Replace($temporary, $statePath, [NullString]::Value) }
            else { [IO.File]::Move($temporary, $statePath) }
            break
        } catch [IO.IOException] { if ($attempt -ge 19) { throw }; Start-Sleep -Milliseconds 50 }
    }
}

function Add-LifecycleTarget {
    param([int]$ProcessId, [string]$Role)
    if ($targets.Count -ge 256) { throw 'lifecycle_process_history_limit_reached' }
    $process = [Diagnostics.Process]::GetProcessById($ProcessId)
    # Retain the native handle before exit, so a missing/reused PID cannot lose
    # the original process exit code. Never reopen by PID to obtain that code.
    $null = $process.Handle
    $created = $process.StartTime.ToUniversalTime()
    $identity = Get-CimInstance Win32_Process -Filter ('ProcessId=' + $ProcessId)
    if (-not $identity -or $identity.ExecutablePath -ne $ExpectedExecutable -or $process.HasExited) { throw 'lifecycle_target_identity_mismatch' }
    if ($Role -eq 'runtime' -and $identity.ParentProcessId -ne $GuiPid) { throw 'lifecycle_runtime_parent_mismatch' }
    $record = [ordered]@{
        role=$Role; pid=$ProcessId; parent_pid=$identity.ParentProcessId; created_at=$created.ToString('o')
        attached_at=[DateTimeOffset]::UtcNow.ToString('o'); exited=$false; exit_code_signed=$null
        exit_code_unsigned=$null; exit_code_hex=$null; exit_observed_at=$null
        collector_pid=0; collector_ready=$false; collector_exited=$false; collector_exit_code=$null
        collector_log=''; dump_directory=''; private_bytes=0; working_set=0; handles=0
    }
    $directory = Join-Path $evidence ('{0}-{1}-{2}' -f $Role,$ProcessId,$created.Ticks)
    New-Item -ItemType Directory -Path $directory | Out-Null
    $record.dump_directory = Split-Path $directory -Leaf
    $outPath = Join-Path $directory 'procdump.stdout.log'
    $errorPath = Join-Path $directory 'procdump.stderr.log'
    $collector = $null
    # Bound new collectors; already attached collectors can each write at most
    # two files. Existing dumps are preserved rather than silently deleted.
    if (@(Get-ChildItem -LiteralPath $evidence -Recurse -File -Filter '*.dmp').Count -lt 8) {
        $arguments = @('-accepteula','-mm','-e','-t','-n','2',"$ProcessId",$directory)
        $collector = Start-Process -FilePath $ProcDumpPath -ArgumentList (($arguments | ForEach-Object { ConvertTo-UpgradeArgument $_ }) -join ' ') -PassThru -WindowStyle Hidden -RedirectStandardOutput $outPath -RedirectStandardError $errorPath
        $null = $collector.Handle
        $record.collector_pid = $collector.Id
        $record.collector_log = Join-Path $record.dump_directory 'procdump.stdout.log'
    }
    $targets.Add([pscustomobject]@{process=$process; collector=$collector; output=$outPath; record=$record})
    Write-LifecycleEvent 'attached' $record
}

try {
    $ProcDumpPath = (Resolve-Path -LiteralPath $ProcDumpPath).Path
    $signature = Get-AuthenticodeSignature -LiteralPath $ProcDumpPath
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') { throw 'lifecycle_procdump_signature_invalid' }
    Write-LifecycleEvent 'monitor_started' @{monitor_pid=$PID; task_name=$TaskName}
    Write-LifecycleState
    if ($GuiPid -eq 0) {
        $phase = 'starting_host'
        Write-LifecycleState
        $shell = Join-Path $PSHOME 'pwsh.exe'
        if (-not (Test-Path -LiteralPath $shell)) { $shell = Join-Path $PSHOME 'powershell.exe' }
        $outPath = Join-Path $evidence 'host-start.stdout.log'
        $errorPath = Join-Path $evidence 'host-start.stderr.log'
        $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'start-debug-host.ps1'))
        $launcher = Start-Process -FilePath $shell -ArgumentList (($arguments | ForEach-Object { ConvertTo-UpgradeArgument $_ }) -join ' ') -WorkingDirectory $repoRoot -PassThru -WindowStyle Hidden -RedirectStandardOutput $outPath -RedirectStandardError $errorPath
        while (-not $launcher.WaitForExit(2000)) { Write-LifecycleState }
        if ($launcher.ExitCode -ne 0) { throw ('lifecycle_host_start_failed_' + $launcher.ExitCode) }
        $text = [IO.File]::ReadAllText($outPath)
        $match = [regex]::Match($text, '(?s)\{\s*"schema"\s*:\s*"redclaw\.cross-lan\.gui\.supervisor\.result\.v1".*\}\s*$')
        if (-not $match.Success) { throw 'lifecycle_host_start_receipt_missing' }
        $receipt = $match.Value | ConvertFrom-Json
        $GuiPid = [int]$receipt.app_pid
        $StatusPath = [string]$receipt.status_path
        $ExpectedExecutable = Join-Path $repoRoot 'release\Debug\redclaw_desktop.exe'
    }
    $ExpectedExecutable = (Resolve-Path -LiteralPath $ExpectedExecutable).Path
    Add-LifecycleTarget $GuiPid 'gui'
    if ($launchHost) {
        $artifacts = Join-Path $evidence 'symbols'
        New-Item -ItemType Directory -Path $artifacts | Out-Null
        foreach ($extension in @('.exe','.pdb')) {
            $artifact = [IO.Path]::ChangeExtension($ExpectedExecutable,$extension)
            if (Test-Path -LiteralPath $artifact) { Copy-Item -LiteralPath $artifact -Destination $artifacts }
        }
        $manifestPath = Join-Path (Split-Path -Parent $StatusPath) 'run-manifest.json'
        if (Test-Path -LiteralPath $manifestPath) { Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $evidence 'runtime-manifest.json') }
    }
    $phase = 'monitoring'
    do {
        if (Test-Path -LiteralPath $StatusPath) {
            try {
                $snapshot = Read-LifecycleText $StatusPath | ConvertFrom-Json
                if ($snapshot.app_pid -and $snapshot.app_pid -ne $GuiPid) { throw 'lifecycle_status_identity_mismatch' }
                $runtimePid = [int]$snapshot.runtime_pid
                if ($runtimePid -gt 0 -and @($targets | Where-Object { $_.record.pid -eq $runtimePid }).Count -eq 0) {
                    Add-LifecycleTarget $runtimePid 'runtime'
                }
                $lastSnapshot = [ordered]@{
                    observed_at=[DateTimeOffset]::UtcNow.ToString('o'); updated_at_unix_ms=$snapshot.updated_at_unix_ms
                    phase=$snapshot.phase; connected=$snapshot.connected; channel_open=$snapshot.channel_open
                    runtime_pid=$runtimePid; dht_reachable=$snapshot.dht_reachable
                    publish_success=$snapshot.dht.publish_success; publish_revision=$snapshot.dht.publish_revision
                    publish_expiry=$snapshot.dht.publish_expiry; publish_expired_total=$snapshot.dht.publish_expired_total
                    fetch_hits=$snapshot.dht.fetch_hits; remote_revision=$snapshot.dht.remote_revision
                    captured=$snapshot.stream.captured; encoded=$snapshot.stream.encoded; transmitted=$snapshot.stream.transmitted
                    synthetic=$snapshot.stream.synthetic; capture_failures=$snapshot.stream.capture_failures
                    encode_failures=$snapshot.stream.encode_failures; transmit_failures=$snapshot.stream.transmit_failures
                }
            } catch [IO.IOException] { Write-LifecycleEvent 'status_read_retry' @{error='io'} }
            catch [ArgumentException] { Write-LifecycleEvent 'status_read_retry' @{error='json'} }
        }
        foreach ($target in $targets) {
            if (-not $target.record.exited -and $target.process.WaitForExit(0)) {
                $code = [int]$target.process.ExitCode
                $unsigned = [uint32]([long]$code -band 4294967295L)
                $target.record.exited = $true
                $target.record.exit_code_signed = $code
                $target.record.exit_code_unsigned = $unsigned
                $target.record.exit_code_hex = '0x{0:X8}' -f $unsigned
                $target.record.exit_observed_at = [DateTimeOffset]::UtcNow.ToString('o')
                Write-LifecycleEvent 'process_exited' $target.record
                if ($target.record.role -eq 'gui') { $phase = 'gui_exited' }
            } elseif (-not $target.record.exited) {
                $target.process.Refresh()
                $target.record.private_bytes = $target.process.PrivateMemorySize64
                $target.record.working_set = $target.process.WorkingSet64
                $target.record.handles = $target.process.HandleCount
            }
            if ($target.collector) {
                if (-not $target.record.collector_ready -and (Test-Path -LiteralPath $target.output)) {
                    $log = Read-LifecycleText $target.output
                    if ($log -match 'Press Ctrl-C') {
                        $target.record.collector_ready = $true
                        Write-LifecycleEvent 'collector_armed' @{role=$target.record.role; pid=$target.record.pid; collector_pid=$target.record.collector_pid}
                    }
                }
                if (-not $target.record.collector_exited -and $target.collector.WaitForExit(0)) {
                    $target.record.collector_exited = $true
                    $target.record.collector_exit_code = $target.collector.ExitCode
                    Write-LifecycleEvent 'collector_exited' @{role=$target.record.role; pid=$target.record.pid; exit_code=$target.collector.ExitCode}
                }
            }
        }
        if (@($targets | Where-Object { -not $_.record.exited }).Count -eq 0) { $phase = 'completed' }
        Write-LifecycleState
        $sequence++
        if ($sequence % 15 -eq 0) { Write-LifecycleEvent 'heartbeat' @{phase=$phase; targets=$targets.Count} }
        if (Test-Path -LiteralPath (Join-Path $evidence 'stop-monitor.request')) { $phase='stopped_by_request'; break }
        if ($phase -ne 'completed') { Start-Sleep -Seconds 2 }
    } while ($phase -ne 'completed')
} catch {
    $phase = 'failed'
    $failure = $_.Exception.Message
    Write-LifecycleEvent 'monitor_failed' @{error=$failure}
} finally {
    # Official cancellation detaches the debugger and resumes any active dump;
    # never kill a live debugger process or the observed application on cleanup.
    foreach ($target in $targets) {
        if ($target.collector -and -not $target.collector.WaitForExit(0)) {
            if (-not $target.process.WaitForExit(0)) {
                $cancelArguments = @('-accepteula','-cancel',"$($target.record.pid)")
                $cancel = Start-Process -FilePath $ProcDumpPath -ArgumentList $cancelArguments -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $evidence ('cancel-' + $target.record.pid + '.log')) -RedirectStandardError (Join-Path $evidence ('cancel-' + $target.record.pid + '.stderr.log'))
                [void]$cancel.WaitForExit(10000)
            }
            [void]$target.collector.WaitForExit(30000)
        }
    }
    Write-LifecycleState
    Copy-Item -LiteralPath $statePath -Destination (Join-Path $evidence 'monitor-result.json') -Force
    if ($TaskName -like 'RedClaw-Diagnostics-*') {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    }
    foreach ($target in $targets) { $target.process.Dispose(); if ($target.collector) { $target.collector.Dispose() } }
}
if ($phase -eq 'failed') { exit 1 }
