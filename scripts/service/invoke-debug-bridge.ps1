param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        'bridge_status',
        'remote_bridge_status',
        'status',
        'start',
        'reconnect',
        'stop',
        'tail_log',
        'remote_log_snapshot',
        'remote_log_read',
        'remote_control_start',
        'remote_control_pause',
        'agent_status',
        'agent_start_fixture',
        'agent_follow_up_fixture',
        'agent_interrupt',
        'agent_approval',
        'agent_evidence',
        'export_evidence',
        'agent_events',
        'agent_task_create',
        'agent_turn_start',
        'agent_turn_steer',
        'agent_turn_interrupt',
        'agent_task_sync')]
    [string]$Action,

    [string]$ControlName = 'RedClawDesktop.DebugBridge.v1',

    [ValidateRange(100, 120000)]
    [int]$TimeoutMs = 10000,

    [ValidateSet('host', 'controller')]
    [string]$Role = 'host',

    [ValidateRange(1, 200)]
    [int]$Limit = 100,

    [ValidateSet('inspect_project', 'write_marker_and_test')]
    [string]$FixtureId = 'inspect_project',

    [ValidateSet('codex', 'cursor')]
    [string]$Provider = 'codex',

    [ValidateSet('accept', 'reject')]
    [string]$Decision = 'accept',

    [ValidatePattern('^[A-Za-z0-9_.-]{0,128}$')]
    [string]$TaskId = '',

    [ValidatePattern('^[A-Za-z0-9_.-]{0,128}$')]
    [string]$ProjectId = '',

    [ValidatePattern('^[A-Za-z0-9_.-]{0,128}$')]
    [string]$ApprovalRequestId = '',

    [string]$Model = '',

    [ValidateSet('isolated_worktree', 'direct_workspace')]
    [string]$WorkDirectoryMode = 'isolated_worktree',

    [string]$Instruction = '',

    [switch]$Json
)

$ErrorActionPreference = 'Stop'

if ($Instruction.Length -gt 16384) {
    throw 'Agent instruction exceeds the 16 KiB UTF-16 input guard.'
}

$request = [ordered]@{
    schema = 'redclaw.debug-bridge-control.v1'
    request_id = [guid]::NewGuid().ToString('N')
    action = $Action
}
$arguments = [ordered]@{}
switch ($Action) {
    'start' { $arguments.role = $Role }
    { $_ -in @('tail_log', 'remote_log_read') } { $arguments.limit = $Limit }
    'agent_start_fixture' {
        $arguments.fixture_id = $FixtureId
        $arguments.provider = $Provider
    }
    'agent_approval' {
        if (-not [string]::IsNullOrWhiteSpace($TaskId)) {
            $arguments.task_id = $TaskId
            $arguments.approval_request_id = $ApprovalRequestId
        }
        $arguments.decision = $Decision
    }
    { $_ -in @('agent_task_create', 'agent_turn_start', 'agent_turn_steer') } {
        $arguments.task_id = $TaskId
        $arguments.instruction = $Instruction
        if ($Action -eq 'agent_task_create') {
            $arguments.project_id = $ProjectId
            $arguments.provider = $Provider
            $arguments.model = $Model
            $arguments.work_directory_mode = $WorkDirectoryMode
        }
    }
    { $_ -in @('agent_turn_interrupt', 'agent_task_sync') } {
        $arguments.task_id = $TaskId
    }
}
if ($arguments.Count -gt 0) {
    $request.arguments = $arguments
}

$pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    '.',
    $ControlName,
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::None)
try {
    $pipe.Connect($TimeoutMs)
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    $writer = [System.IO.StreamWriter]::new($pipe, $utf8, 4096, $true)
    $reader = [System.IO.StreamReader]::new($pipe, $utf8, $false, 4096, $true)
    try {
        $writer.AutoFlush = $true
        $writer.WriteLine(($request | ConvertTo-Json -Depth 8 -Compress))
        $readTask = $reader.ReadLineAsync()
        if (-not $readTask.Wait($TimeoutMs)) {
            throw "Debug bridge response timed out after $TimeoutMs ms."
        }
        $responseLine = $readTask.GetAwaiter().GetResult()
    } finally {
        $reader.Dispose()
        $writer.Dispose()
    }
} finally {
    $pipe.Dispose()
}

if ([string]::IsNullOrWhiteSpace($responseLine)) {
    throw 'Debug bridge returned an empty response.'
}
$response = $responseLine | ConvertFrom-Json
if ($Json) {
    $response | ConvertTo-Json -Depth 20
} else {
    $response | Format-List
}
if ($null -ne $response.ok -and -not [bool]$response.ok) {
    exit 1
}
