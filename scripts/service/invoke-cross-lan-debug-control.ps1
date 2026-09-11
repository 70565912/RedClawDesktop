param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        'status',
        'start',
        'reconnect',
        'stop',
        'exit',
        'tail_log',
        'remote_log_snapshot',
        'remote_log_read',
        'remote_control_start',
        'remote_control_pause',
        'remote_input_mouse_click',
        'measurement_arm',
        'measurement_export',
        'input_probe_start',
        'input_probe_stop',
        'input_probe_export',
        'remote_input_key_press',
        'agent_status',
        'agent_start_fixture',
        'agent_follow_up_fixture',
        'agent_interrupt',
        'agent_approval',
        'agent_evidence',
        'export_evidence')]
    [string]$Action,

    [ValidateSet('host', 'controller')]
    [string]$Role = 'host',

    [ValidateRange(1, 200)]
    [int]$Limit = 100,

    [ValidateRange(0, 65535)]
    [int]$NormalizedX = 32768,

    [ValidateRange(0, 65535)]
    [int]$NormalizedY = 32768,

    [ValidateSet('left', 'right', 'middle', 'x1', 'x2')]
    [string]$MouseButton = 'left',

    [ValidateRange(1, 255)]
    [int]$ScanCode = 30,

    [ValidateRange(0, 255)]
    [int]$VirtualKey = 65,

    [switch]$Extended,

    [ValidateSet('inspect_project', 'write_marker_and_test')]
    [string]$FixtureId = 'inspect_project',

    [ValidateSet('codex', 'cursor')]
    [string]$Provider = 'codex',

    [ValidateSet('accept', 'reject')]
    [string]$Decision = 'accept',

    [string]$ControlName = 'RedClawDesktop.DebugControl.v1',

    [ValidateRange(100, 30000)]
    [int]$TimeoutMs = 5000,

    [ValidateRange(100, 120000)]
    [int]$EvidenceTimeoutMs = 30000,

    [ValidateRange(100, 30000)]
    [int]$RemoteControlActivationTimeoutMs = 5000,

    [ValidateRange(0, 3600)]
    [int]$WaitTimeoutSeconds = 0,

    [ValidateSet('idle', 'starting', 'dht_waiting', 'remote_description', 'ice_connecting', 'connected', 'channel_open', 'streaming', 'failed', 'stopped')]
    [string[]]$WaitForPhase = @(),

    [switch]$Json
)

$ErrorActionPreference = 'Stop'

function Invoke-DebugControlRequest {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Request
    )

    $responseTimeoutMs = if ([string]$Request.action -eq 'export_evidence') {
        $EvidenceTimeoutMs
    } else {
        $TimeoutMs
    }
    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.',
        $ControlName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    try {
        $pipe.Connect($TimeoutMs)
        $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
        $writer = [System.IO.StreamWriter]::new($pipe, $utf8NoBom, 4096, $true)
        $reader = [System.IO.StreamReader]::new($pipe, $utf8NoBom, $false, 4096, $true)
        try {
            $writer.AutoFlush = $true
            $writer.WriteLine(($Request | ConvertTo-Json -Depth 6 -Compress))
            $readTask = $reader.ReadLineAsync()
            if (-not $readTask.Wait($responseTimeoutMs)) {
                throw "Debug control response timed out after $responseTimeoutMs ms."
            }
            $line = $readTask.GetAwaiter().GetResult()
            if ([string]::IsNullOrWhiteSpace($line)) {
                throw 'Debug control returned an empty response.'
            }
            return ($line | ConvertFrom-Json)
        } finally {
            $reader.Dispose()
            $writer.Dispose()
        }
    } finally {
        $pipe.Dispose()
    }
}

function New-Request {
    param([string]$RequestAction)

    $request = @{
        schema = 'redclaw.debug-control.v1'
        request_id = [guid]::NewGuid().ToString('N')
        action = $RequestAction
    }
    if ($RequestAction -eq 'start') {
        $request.role = $Role
    }
    if ($RequestAction -eq 'tail_log' -or $RequestAction -eq 'remote_log_read') {
        $request.limit = $Limit
    }
    if ($RequestAction -eq 'remote_input_mouse_click') {
        $request.x = $NormalizedX
        $request.y = $NormalizedY
        $request.button = $MouseButton
    }
    if ($RequestAction -eq 'remote_input_key_press') {
        $request.scan_code = $ScanCode
        $request.virtual_key = $VirtualKey
        $request.extended = [bool]$Extended
    }
    if ($RequestAction -eq 'agent_start_fixture') {
        $request.fixture_id = $FixtureId
        $request.provider = $Provider
    }
    if ($RequestAction -eq 'agent_approval') {
        $request.decision = $Decision
    }
    return $request
}

try {
    $response = Invoke-DebugControlRequest -Request (New-Request -RequestAction $Action)
} catch {
    Write-Error ("Debug control transport failed: {0}" -f $_.Exception.Message)
    exit 2
}

if (-not [bool]$response.ok) {
    if ($Json) {
        $response | ConvertTo-Json -Depth 10
    } else {
        Write-Host ("[debug-control] action={0} ok=false error_code={1} detail={2}" -f $Action, $response.error_code, $response.error_detail)
    }
    exit 1
}

if ($Action -eq 'remote_control_start') {
    $activationDeadline = [DateTimeOffset]::UtcNow.AddMilliseconds(
        $RemoteControlActivationTimeoutMs)
    while (-not [bool]$response.result.remote_input.active -and
        [bool]$response.result.remote_input.request_pending -and
        [DateTimeOffset]::UtcNow -lt $activationDeadline) {
        Start-Sleep -Milliseconds 100
        try {
            $response = Invoke-DebugControlRequest -Request (New-Request -RequestAction 'status')
        } catch {
            Write-Error ("Debug control activation wait failed: {0}" -f $_.Exception.Message)
            exit 2
        }
        if (-not [bool]$response.ok) {
            break
        }
    }
    if (-not [bool]$response.ok -or -not [bool]$response.result.remote_input.active) {
        if ($Json) {
            $response | ConvertTo-Json -Depth 10
        } else {
            Write-Host ("[debug-control] remote control activation failed state={0} reason={1} pending={2}" -f `
                $response.result.remote_input.state,
                $response.result.remote_input.reason,
                $response.result.remote_input.request_pending)
        }
        exit 3
    }
}

if ($WaitTimeoutSeconds -gt 0 -and $WaitForPhase.Count -gt 0) {
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($WaitTimeoutSeconds)
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        if (([string]$response.status.phase) -in $WaitForPhase) {
            break
        }
        Start-Sleep -Seconds 1
        try {
            $response = Invoke-DebugControlRequest -Request (New-Request -RequestAction 'status')
        } catch {
            Write-Error ("Debug control status wait failed: {0}" -f $_.Exception.Message)
            exit 2
        }
        if (-not [bool]$response.ok) {
            break
        }
    }
    if (([string]$response.status.phase) -notin $WaitForPhase) {
        if ($Json) {
            $response | ConvertTo-Json -Depth 10
        } else {
            Write-Host ("[debug-control] wait timeout phase={0} expected={1}" -f $response.status.phase, ($WaitForPhase -join ','))
        }
        exit 3
    }
}

if ($Json) {
    $response | ConvertTo-Json -Depth 10
} else {
    Write-Host ("[debug-control] action={0} ok=true phase={1} role={2} runtime_pid={3}" -f `
        $Action, $response.status.phase, $response.status.role, $response.status.runtime_pid)
    if ($Action -eq 'export_evidence') {
        Write-Host ("[debug-control] manifest={0} sha256={1}" -f `
            $response.result.manifest_path, $response.result.manifest_sha256)
    }
    if ($Action -eq 'tail_log') {
        foreach ($line in @($response.result.lines)) {
            Write-Host $line
        }
    }
    if ($Action -eq 'remote_log_snapshot') {
        Write-Host ("[debug-control] peer_request_id={0}" -f $response.result.peer_request_id)
    }
    if ($Action -eq 'remote_log_read') {
        Write-Host ("[debug-control] peer_request_id={0} complete={1} error={2}" -f `
            $response.result.peer_request_id, $response.result.complete, $response.result.remote_error)
        foreach ($line in @($response.result.lines)) {
            Write-Host $line
        }
    }
    if ($null -ne $response.result.remote_input) {
        Write-Host ("[debug-control] remote_input active={0} pending={1} supported={2} authorized={3} sent_batches={4} ack_rtt_ms={5}" -f `
            $response.result.remote_input.active,
            $response.result.remote_input.request_pending,
            $response.result.remote_input.supported,
            $response.result.remote_input.authorized,
            $response.result.remote_input.sent_batches,
            $response.result.remote_input.application_ack_rtt_ms)
    }
    if ($Action -in @('agent_status', 'agent_start_fixture', 'agent_follow_up_fixture', 'agent_approval', 'agent_evidence')) {
        Write-Host ("[debug-control] agent task_id={0} status={1} provider_count={2} project_count={3} approval_pending={4}" -f `
            $response.result.task_id,
            $response.result.status,
            $response.result.provider_count,
            $response.result.project_count,
            $response.result.approval_pending)
        foreach ($providerStatus in @($response.result.providers)) {
            Write-Host ("[debug-control] provider name={0} available={1} readiness={2} version={3}" -f `
                $providerStatus.name,
                $providerStatus.available,
                $providerStatus.readiness,
                $providerStatus.version)
        }
    }
}

exit 0
