param(
    [Parameter(Mandatory = $true)][string]$RunDirectory,
    [Parameter(Mandatory = $true)][string]$ContractFile,
    [Parameter(Mandatory = $true)][string]$ReportDirectory,
    [ValidateSet('Static', 'Dynamic', 'DynamicLog')]
    [string[]]$Scenario = @('Static', 'Dynamic', 'DynamicLog'),
    [ValidateRange(1, 3)][int]$Repetitions = 3,
    [int]$RandomSeed = 2700,
    [switch]$PrepareOnly
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$contract = Get-Content -LiteralPath $ContractFile -Raw | ConvertFrom-Json
foreach ($field in 'capture_width','capture_height','encode_width','encode_height','viewport_width','viewport_height') {
    if ([int]$contract.$field -le 0) { throw "Missing positive geometry contract: $field" }
}
if ($null -eq $contract.log_visible -or $null -eq $contract.mirror_visible -or
    -not $contract.network_configuration_id -or [int]$contract.diagnostic_interval_ms -le 0) {
    throw 'The contract must fix both log visibilities, network configuration identity and diagnostic interval.'
}
if (Test-Path -LiteralPath $ReportDirectory) { throw 'Use a new directory to preserve earlier evidence.' }
New-Item -ItemType Directory -Path $ReportDirectory | Out-Null
$report = (Resolve-Path -LiteralPath $ReportDirectory).Path
$plan = [ordered]@{
    schema = 'redclaw.video-link-baseline.v1'; warmup_seconds = 10; measurement_seconds = 60
    repetitions = $Repetitions; scenarios = $Scenario; random_seed = $RandomSeed
    scene_fps = 30; scene_fullscreen = $true; scene_top_most = $true
    contract = $contract; status_poll_ms = 1000
    executed = $false; cross_machine_timestamp_subtraction = $false
}
$plan | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $report 'plan.json') -Encoding UTF8
if ($PrepareOnly) { Write-Host 'Baseline plan validated. No load or sampling started.'; return }

$manifest = Get-Content -LiteralPath (Join-Path $RunDirectory 'run-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.role -ne 'host' -or $manifest.configuration -ne 'Debug' -or $manifest.signal_transport -ne 'dht') {
    throw 'A controlled local Debug Host using DHT is required.'
}
if ((Get-FileHash -LiteralPath $manifest.runtime_path -Algorithm SHA256).Hash -ne $manifest.runtime_sha256) {
    throw 'The executable no longer matches the startup identity.'
}
Copy-Item -LiteralPath (Join-Path $RunDirectory 'run-manifest.json') -Destination (Join-Path $report 'program.json')
$qa = Join-Path $repo 'scripts/service/invoke-cross-lan-debug-control.ps1'
function Invoke-Qa([string]$Action) {
    $reply = & $qa -ControlName $manifest.control_name -Action $Action -Json | ConvertFrom-Json
    if (-not $reply.ok) { throw "Local Debug request failed: $Action" }
    return $reply
}
function Read-Ready {
    $reply = Invoke-Qa 'status'
    $status = $reply.status
    # Runtime state and stream stats arrive separately. A healthy Host can
    # transiently report "connected" between those records, including at idle.
    if ($status.role -ne 'host' -or -not $status.runtime_running -or -not $status.connected -or
        -not $status.channel_open -or $status.phase -notin @('connected','channel_open','streaming') -or
        [long]$status.stream.transmitted -le 0 -or
        [long]$status.stream.captured -le 0 -or [long]$status.stream.synthetic -ne 0) {
        Save-Snapshot $reply (Join-Path $report 'failed-readiness.json')
        throw 'Wait for the remote Client and a real captured media stream before starting the baseline.'
    }
    if ($null -eq $reply.result.runtime_log -or
        [bool]$reply.result.runtime_log.visible -ne [bool]$contract.log_visible -or
        [bool]$reply.result.runtime_log.mirror_visible -ne [bool]$contract.mirror_visible) {
        throw 'Candidate log visibility does not match the fixed baseline contract.'
    }
    return $reply
}
function Save-Snapshot($Reply, [string]$Path) {
    $metrics = foreach ($processId in @($Reply.status.app_pid, $Reply.status.runtime_pid)) {
        $process = Get-Process -Id $processId -ErrorAction Stop
        [pscustomobject]@{id=$processId; cpu_seconds=$process.CPU; working_set_bytes=$process.WorkingSet64;
            private_bytes=$process.PrivateMemorySize64}
    }
    [pscustomobject]@{schema='redclaw.video-link-sample.v1'; observed_utc=[DateTimeOffset]::UtcNow.ToString('o');
        status=$Reply.status; local_log=$Reply.result.runtime_log; processes=@($metrics)} |
        ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $Path -Encoding UTF8
}
$initial = Read-Ready
$runtimeId = $initial.status.runtime_pid
$runtimeLogs = @(Get-ChildItem -LiteralPath $RunDirectory -Filter "redclaw-desktop-host-*-$runtimeId.log")
if ($runtimeLogs.Count -ne 1) { throw 'The current Host runtime log/trace source is missing or ambiguous.' }
$runtimeLog = $runtimeLogs[0].FullName
$windowsPowerShell = Join-Path ([Environment]::GetFolderPath('Windows')) 'System32/WindowsPowerShell/v1.0/powershell.exe'
$windows = [Collections.Generic.List[object]]::new()
foreach ($scene in $Scenario) {
    foreach ($trial in 1..$Repetitions) {
        $ready = Read-Ready
        if ($ready.status.runtime_pid -ne $runtimeId) { throw 'Host restarted; establish a new baseline identity.' }
        $destination = Join-Path $report ("$scene-$trial")
        New-Item -ItemType Directory -Path $destination | Out-Null
        $sceneReady = Join-Path $destination 'scene-ready.json'
        $sceneReport = Join-Path $destination 'scene.json'
        $sceneKind = if ($scene -eq 'Static') {'Static'} else {'Dynamic'}
        $sceneScript = Join-Path $PSScriptRoot 'run-local-high-motion-scene.ps1'
        # These are local owned paths. Quoting is only for Start-Process argument joining.
        $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$sceneScript+'"'),
            '-DurationSeconds','120','-TargetFps','30','-RandomSeed',$RandomSeed,'-Scene',$sceneKind,
            '-Fullscreen','-TopMost',
            '-Width',$contract.capture_width,'-Height',$contract.capture_height,
            '-ReportFile',('"'+$sceneReport+'"'),'-ReadyFile',('"'+$sceneReady+'"'))
        $sceneProcess = Start-Process -FilePath $windowsPowerShell -ArgumentList $arguments -WindowStyle Hidden -PassThru
        try {
            $startWait = [Diagnostics.Stopwatch]::StartNew()
            while (-not (Test-Path -LiteralPath $sceneReady)) {
                if ($sceneProcess.HasExited -or $startWait.Elapsed.TotalSeconds -gt 15) { throw 'Desktop scene did not become ready.' }
                Start-Sleep -Milliseconds 100
            }
            if ($scene -eq 'DynamicLog') { [void](Invoke-Qa 'log_replay_start') }
            Start-Sleep -Seconds 10
            $request = $runtimeLog + '.frame-trace.request'
            if (Test-Path -LiteralPath $request) { throw 'Another Host trace request is pending.' }
            $existing = @(Get-ChildItem -LiteralPath (Split-Path $runtimeLog) -Filter '*.frame-trace-*.csv' | Select-Object -ExpandProperty FullName)
            $consumedBefore = @(Get-ChildItem -LiteralPath (Split-Path $request) -Filter ((Split-Path $request -Leaf)+'.consumed-*') | Select-Object -ExpandProperty FullName)
            [IO.File]::WriteAllText($request, 'redclaw.host-frame-trace.v1 60')
            # The Host polls this sidecar on its 10 s diagnostic tick. Wait for
            # consumption so the 60 s scene window covers the actual trace.
            $armWait = [Diagnostics.Stopwatch]::StartNew()
            do {
                $consumed = @(Get-ChildItem -LiteralPath (Split-Path $request) -Filter ((Split-Path $request -Leaf)+'.consumed-*') |
                    Where-Object { $_.FullName -notin $consumedBefore })
                if ($consumed.Count -eq 1) { break }
                Start-Sleep -Milliseconds 100
            } while ($armWait.Elapsed.TotalSeconds -lt 15)
            if ($consumed.Count -ne 1) { throw 'Host trace request was not consumed.' }
            [pscustomobject]@{schema='redclaw.video-link-arm.v1';minimum_warmup_seconds=10;
                diagnostic_barrier_seconds=$armWait.Elapsed.TotalSeconds;
                host_trace_started_us=[long]($consumed[0].Name -replace '^.*\.consumed-','')} |
                ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'host-arm.json') -Encoding UTF8
            $before = Read-Ready
            Save-Snapshot $before (Join-Path $destination 'start.json')
            Invoke-Qa 'measurement_arm' | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $destination 'gui-arm.json')
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $index = 0
            do {
                Start-Sleep -Seconds 1
                $after = Read-Ready
                if ($after.status.runtime_pid -ne $runtimeId) { throw 'Host changed during the measurement.' }
                Save-Snapshot $after (Join-Path $destination ("sample-{0:D3}.json" -f $index++))
            } while ($clock.Elapsed.TotalSeconds -lt 60)
            $seconds = ([long]$after.status.gui_scheduling.sampled_at_us -
                [long]$before.status.gui_scheduling.sampled_at_us) / 1000000.0
            if ($scene -eq 'DynamicLog') {
                Invoke-Qa 'log_replay_stop' | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $destination 'log-replay.json')
            }
            Invoke-Qa 'measurement_export' | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $destination 'gui-export.json')
            # The existing recorder drains in-flight frames for 11 s after its window.
            $drain = [Diagnostics.Stopwatch]::StartNew()
            $traceInput = $null
            do {
                $trace = @(Get-ChildItem -LiteralPath (Split-Path $runtimeLog) -Filter '*.frame-trace-*.csv' |
                    Where-Object { $_.FullName -notin $existing -and $_.FullName.StartsWith($runtimeLog + '.frame-trace-') })
                if ($trace.Count -eq 1) {
                    try {
                        # Older Hosts create the CSV before completing its write.
                        # Exclusive access is available only after that writer closes.
                        $traceInput = [IO.File]::Open($trace[0].FullName, [IO.FileMode]::Open,
                            [IO.FileAccess]::Read, [IO.FileShare]::None)
                        break
                    } catch [IO.IOException] { }
                }
                Start-Sleep -Milliseconds 500
            } while ($drain.Elapsed.TotalSeconds -lt 30)
            if ($null -eq $traceInput) { throw 'Host frame trace did not finish writing.' }
            $traceCopy = Join-Path $destination 'host-frames.csv'
            try {
                $traceOutput = [IO.File]::Create($traceCopy)
                try { $traceInput.CopyTo($traceOutput) } finally { $traceOutput.Dispose() }
            } finally { $traceInput.Dispose() }
            $header = Get-Content -LiteralPath $traceCopy -TotalCount 1
            if ($header -notmatch 'overflow=0$') { throw 'Trace overflow makes this window invalid.' }
            $frames = @(Get-Content -LiteralPath $traceCopy | Select-Object -Skip 1 | ConvertFrom-Csv)
            foreach ($frame in $frames) {
                if (@($frame.PSObject.Properties | Where-Object { $_.Value -notmatch '^\d+$' }).Count -ne 0) {
                    throw 'Incomplete or nonnumeric Host trace row.'
                }
            }
            if (($scene -ne 'Static' -and $frames.Count -eq 0) -or @($frames | Where-Object {
                [int]$_.width -ne [int]$contract.encode_width -or [int]$_.height -ne [int]$contract.encode_height
            }).Count -ne 0) { throw 'Empty dynamic trace or encoded geometry changed.' }
            # An unchanged desktop legitimately has no new frame to encode.
            # Verify its retained geometry from runtime stats, without filler.
            $geometry = Get-Content -LiteralPath $runtimeLog -Tail 500 |
                Where-Object { $_ -match 'Runtime desktop stream stats' } | Select-Object -Last 1
            foreach ($field in @('source_width','source_height','encoded_width','encoded_height')) {
                $expected = switch ($field) {
                    'source_width' {$contract.capture_width}; 'source_height' {$contract.capture_height}
                    'encoded_width' {$contract.encode_width}; 'encoded_height' {$contract.encode_height}
                }
                if ($geometry -notmatch ("(?:^| )"+$field+'=(\d+)') -or [int]$Matches[1] -ne [int]$expected) {
                    throw "Runtime geometry changed or missing: $field"
                }
            }
            $geometry | Set-Content -LiteralPath (Join-Path $destination 'host-geometry.txt') -Encoding UTF8
            # These fixed-prefix numeric summaries preserve effective codec and stage
            # parameters without copying unrelated runtime/Agent output.
            $diagnostics = foreach ($suffix in '.2','.1','') {
                if (Test-Path -LiteralPath ($runtimeLog+$suffix)) {
                    Get-Content -LiteralPath ($runtimeLog+$suffix) -Tail 500 |
                        Where-Object { $_ -match 'Runtime host stream (diagnostics|stage)' }
                }
            }
            $diagnostics | Set-Content -LiteralPath (Join-Path $destination 'host-stage-summaries.txt') -Encoding UTF8
            $windows.Add([pscustomobject]@{scene=$scene; trial=$trial; seconds=$seconds; frame_count=$frames.Count;
                captured_delta=([long]$after.status.stream.captured-[long]$before.status.stream.captured);
                encoded_delta=([long]$after.status.stream.encoded-[long]$before.status.stream.encoded);
                sent_delta=([long]$after.status.stream.transmitted-[long]$before.status.stream.transmitted)})
        } finally {
            try {
                if ($scene -eq 'DynamicLog') { [void](Invoke-Qa 'log_replay_stop') }
            } finally {
                # Only the scene process created by this iteration may be closed.
                if (-not $sceneProcess.HasExited) {
                    [void]$sceneProcess.CloseMainWindow()
                    if (-not $sceneProcess.WaitForExit(5000)) { Stop-Process -Id $sceneProcess.Id }
                }
            }
        }
    }
}
[pscustomobject]@{schema='redclaw.video-link-baseline.v1'; ok=$true; windows=$windows;
    scope='local Host metrics only; remote Client and visual quality evidence remain separate'} |
    ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $report 'result.json') -Encoding UTF8
Write-Host 'Host baseline windows saved. Compare three-run medians/ranges; do not subtract peer wall clocks.'
