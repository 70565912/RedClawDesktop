param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('create', 'turn', 'steer', 'interrupt', 'approval', 'sync', 'ack', 'status', 'evidence')]
    [string]$Operation,

    [string]$TaskId = '',

    [string]$RequestId = '',

    [ValidateSet('none', 'codex', 'cursor')]
    [string]$Provider = 'none',

    [string]$Model = '',

    [string]$ProjectId = '',

    [ValidateSet('isolated_worktree', 'direct_workspace')]
    [string]$WorkDirectoryMode = 'isolated_worktree',

    [string]$Instruction = '',

    [string]$InstructionFile = '',

    [ValidateSet('accept', 'reject')]
    [string]$Decision = 'reject',

    [string]$SupersedesRequestId = '',

    [ValidateRange(0, [long]::MaxValue)]
    [long]$AcknowledgedEventSequence = 0,

    [string]$EvidenceManifestName = '',

    [string]$EvidenceSha256 = '',

    [string]$ControlName = 'RedClawDesktop.AgentControl.v1',

    [string]$ClientStatePath = '',

    [ValidateRange(100, 30000)]
    [int]$TimeoutMs = 5000,

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$CodecPath = '',

    [switch]$Json
)

$ErrorActionPreference = 'Stop'

$script:MaxInstructionBytes = 16 * 1024
$script:MaxFrameBytes = 64 * 1024

function Protect-OwnerOnlyFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $acl = [System.Security.AccessControl.FileSecurity]::new()
    $acl.SetOwner($identity.User)
    $acl.SetAccessRuleProtection($true, $false)
    $rule = [System.Security.AccessControl.FileSystemAccessRule]::new(
        $identity.User,
        [System.Security.AccessControl.FileSystemRights]::FullControl,
        [System.Security.AccessControl.AccessControlType]::Allow)
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Assert-OwnerOnlyFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $acl = Get-Acl -LiteralPath $Path
    if (-not $acl.AreAccessRulesProtected -or
        $acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value -ne $identity.User.Value) {
        throw 'The local Agent control client state is not owner-local.'
    }
    $currentUserAllowed = $false
    foreach ($rule in $acl.Access) {
        if ($rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow) {
            continue
        }
        $ruleSid = $rule.IdentityReference.Translate(
            [System.Security.Principal.SecurityIdentifier]).Value
        if ($ruleSid -ne $identity.User.Value) {
            throw 'The local Agent control client state grants another principal access.'
        }
        $currentUserAllowed = $true
    }
    if (-not $currentUserAllowed) {
        throw 'The local Agent control client state does not grant current-user access.'
    }
}

function Get-NextClientIdentity {
    $statePath = if ([string]::IsNullOrWhiteSpace($ClientStatePath)) {
        Join-Path $env:LOCALAPPDATA 'RedClawDesktop\coordination\agent-control-client-state-v1.json'
    } else {
        [System.IO.Path]::GetFullPath($ClientStatePath)
    }
    $stateDirectory = Split-Path -Parent $statePath
    $mutex = [System.Threading.Mutex]::new($false, 'Local\RedClawDesktop.AgentControlClient.v1')
    $locked = $false
    try {
        $locked = $mutex.WaitOne(10000)
        if (-not $locked) {
            throw 'Timed out waiting for the local Agent control client state lock.'
        }
        New-Item -ItemType Directory -Force -Path $stateDirectory | Out-Null
        $state = $null
        if (Test-Path -LiteralPath $statePath) {
            Assert-OwnerOnlyFile -Path $statePath
            try {
                $state = Get-Content -Raw -LiteralPath $statePath | ConvertFrom-Json
            } catch {
                throw 'The local Agent control client state is invalid; refusing to reset replay identity.'
            }
        }
        if ($null -eq $state -or [string]$state.schema -ne 'redclaw.agent-control.client-state.v1' -or
            [string]::IsNullOrWhiteSpace([string]$state.session_epoch) -or [long]$state.message_id -lt 0) {
            $state = [pscustomobject]@{
                schema = 'redclaw.agent-control.client-state.v1'
                session_epoch = ('client-' + [guid]::NewGuid().ToString('N'))
                message_id = 0
            }
        }
        $state.message_id = [long]$state.message_id + 1
        $temporary = $statePath + '.tmp'
        try {
            $state | ConvertTo-Json -Compress | Set-Content -LiteralPath $temporary -Encoding UTF8
            Protect-OwnerOnlyFile -Path $temporary
            Move-Item -Force -LiteralPath $temporary -Destination $statePath
        } finally {
            if (Test-Path -LiteralPath $temporary) {
                Remove-Item -Force -LiteralPath $temporary
            }
        }
        return @{
            SessionEpoch = [string]$state.session_epoch
            MessageId = [long]$state.message_id
        }
    } finally {
        if ($locked) {
            $mutex.ReleaseMutex()
        }
        $mutex.Dispose()
    }
}

function Invoke-AgentWireCodec {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('--encode-agent', '--decode-agent')][string]$Mode,
        [Parameter(Mandatory = $true)][string]$InputText
    )
    $codec = $CodecPath
    if ([string]::IsNullOrWhiteSpace($codec)) {
        $repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
        $codec = Join-Path $repository ("release/{0}/redclaw_protocol_codec.exe" -f $Configuration)
    }
    if (-not (Test-Path -LiteralPath $codec -PathType Leaf)) {
        throw 'The matching published Agent Protobuf codec is missing. Build and publish the selected configuration first.'
    }
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = (Resolve-Path -LiteralPath $codec).Path
    $start.Arguments = $Mode
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Unable to start Agent wire codec.' }
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        # Windows PowerShell 5.1 has no ProcessStartInfo.StandardInputEncoding.
        # Write explicit UTF-8 bytes so neither the console code page nor a BOM
        # changes non-ASCII instructions before the native Protobuf encoder.
        $inputBytes = [System.Text.UTF8Encoding]::new($false).GetBytes($InputText)
        $process.StandardInput.BaseStream.Write($inputBytes, 0, $inputBytes.Length)
        $process.StandardInput.BaseStream.Flush()
        $process.StandardInput.Close()
        if (-not $process.WaitForExit($TimeoutMs)) {
            $process.Kill()
            $process.WaitForExit()
            throw 'Agent wire codec timed out.'
        }
        if ($process.ExitCode -ne 0) {
            # Do not echo raw authentication or instruction-bearing input/output.
            throw 'Agent wire codec rejected the message.'
        }
        $value = $output.GetAwaiter().GetResult()
        [void]$errors.GetAwaiter().GetResult()
        if ([System.Text.Encoding]::UTF8.GetByteCount($value) -gt 256 * 1024) {
            throw 'Agent wire codec response exceeds its bounded output size.'
        }
        return $value
    } finally { $process.Dispose() }
}

function New-AgentFrame {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Fields,
        [Parameter(Mandatory = $true)][hashtable]$Identity
    )
    $payload = $Fields.Clone()
    $payload.schema_version = 1
    $payload.session_epoch = $Identity.SessionEpoch
    $payload.message_id = $Identity.MessageId
    $payload.sent_at_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $frame = Invoke-AgentWireCodec -Mode '--encode-agent' -InputText ($payload | ConvertTo-Json -Compress)
    if ([System.Text.Encoding]::UTF8.GetByteCount($frame) -gt $script:MaxFrameBytes) {
        throw 'The local Agent control frame exceeds 65536 bytes.'
    }
    return $frame
}

function Read-AgentFrame {
    param([Parameter(Mandatory = $true)][string]$Frame)
    $json = Invoke-AgentWireCodec -Mode '--decode-agent' -InputText $Frame
    $decoded = $json | ConvertFrom-Json
    $result = @{}
    foreach ($property in $decoded.PSObject.Properties) {
        $result[$property.Name] = $property.Value
    }
    return $result
}

if (-not [string]::IsNullOrWhiteSpace($InstructionFile)) {
    if (-not [string]::IsNullOrWhiteSpace($Instruction)) {
        throw 'Use either -Instruction or -InstructionFile, not both.'
    }
    $Instruction = Get-Content -Raw -LiteralPath $InstructionFile
}
$instructionBytes = [System.Text.Encoding]::UTF8.GetByteCount($Instruction)
if ($instructionBytes -gt $script:MaxInstructionBytes) {
    throw 'Instruction exceeds the 16384-byte Agent protocol limit.'
}

if ($Operation -eq 'approval' -and [string]::IsNullOrWhiteSpace($RequestId)) {
    throw 'approval requires the exact pending -RequestId.'
}
if ([string]::IsNullOrWhiteSpace($RequestId)) {
    $RequestId = 'request-' + [guid]::NewGuid().ToString('N')
}
if ($Operation -eq 'create' -and [string]::IsNullOrWhiteSpace($TaskId)) {
    $TaskId = 'task-' + [guid]::NewGuid().ToString('N')
}
if ($Operation -in @('create', 'turn', 'steer') -and [string]::IsNullOrWhiteSpace($Instruction)) {
    throw "$Operation requires a bounded instruction."
}
if ($Operation -in @('create', 'turn') -and
    ($Provider -eq 'none' -or [string]::IsNullOrWhiteSpace($ProjectId))) {
    throw "$Operation requires -Provider and opaque -ProjectId."
}
if ($Operation -in @('create', 'turn', 'steer', 'interrupt', 'approval', 'sync', 'ack', 'status', 'evidence') -and
    [string]::IsNullOrWhiteSpace($TaskId)) {
    throw "$Operation requires -TaskId."
}
if ($Operation -eq 'evidence' -and
    ($EvidenceManifestName -notmatch '^[A-Za-z0-9._-]{1,256}$' -or
     $EvidenceSha256 -notmatch '^[0-9a-fA-F]{64}$')) {
    throw 'evidence requires a base -EvidenceManifestName and exact -EvidenceSha256.'
}
$type = switch ($Operation) {
    'create' { 'agent_task_create' }
    'turn' { 'agent_turn_start' }
    'steer' { 'agent_turn_steer' }
    'interrupt' { 'agent_turn_interrupt' }
    'approval' { 'agent_approval_decision' }
    'ack' { 'agent_event_ack' }
    default { 'agent_task_sync_request' }
}
$fields = @{
    task_id = $TaskId
    request_id = $RequestId
    supersedes_request_id = $SupersedesRequestId
    message_type = $type
    provider = $Provider
    work_directory_mode = $WorkDirectoryMode
    approval_decision = $(if ($Operation -eq 'approval') { $Decision } else { 'none' })
    model = $Model
    project_id = $ProjectId
    event_kind = $(if ($Operation -in @('status', 'evidence')) { "coordination_$Operation" } else { '' })
    evidence_manifest_name = $(if ($Operation -eq 'evidence') { $EvidenceManifestName } else { '' })
    evidence_sha256 = $(if ($Operation -eq 'evidence') { $EvidenceSha256.ToLowerInvariant() } else { '' })
    acknowledged_event_sequence = $AcknowledgedEventSequence
    text = $Instruction
}

$identity = Get-NextClientIdentity
$frame = New-AgentFrame -Fields $fields -Identity $identity
$pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    '.', $ControlName, [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::None)
try {
    $pipe.Connect($TimeoutMs)
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    $writer = [System.IO.StreamWriter]::new($pipe, $utf8NoBom, 4096, $true)
    $reader = [System.IO.StreamReader]::new($pipe, $utf8NoBom, $false, 4096, $true)
    try {
        $writer.AutoFlush = $true
        $writer.WriteLine($frame)
        $readTask = $reader.ReadLineAsync()
        if (-not $readTask.Wait($TimeoutMs)) {
            throw "Agent control response timed out after $TimeoutMs ms."
        }
        $responseLine = $readTask.GetAwaiter().GetResult()
        if ([string]::IsNullOrWhiteSpace($responseLine)) {
            throw 'Agent control returned an empty response.'
        }
        $response = Read-AgentFrame -Frame $responseLine
    } finally {
        $reader.Dispose()
        $writer.Dispose()
    }
} finally {
    $pipe.Dispose()
}

$result = [pscustomobject]@{
    ok = [string]$response.message_type -ne 'agent_task_error'
    operation = $Operation
    task_id = [string]$response.task_id
    request_id = [string]$response.request_id
    supersedes_request_id = [string]$response.supersedes_request_id
    state = [string]$response.task_state
    event_sequence = [long]$response.event_sequence
    acknowledged_event_sequence = [long]$response.acknowledged_event_sequence
    event_kind = [string]$response.event_kind
    error_code = [string]$response.error_code
    evidence_manifest_name = [string]$response.evidence_manifest_name
    evidence_sha256 = [string]$response.evidence_sha256
    detail = [string]$response.text
}

if ($Json) {
    $result | ConvertTo-Json -Depth 4
} elseif ($result.ok) {
    Write-Host ("[agent-control] operation={0} task_id={1} request_id={2} state={3} event_sequence={4}" -f `
        $Operation, $result.task_id, $result.request_id, $result.state, $result.event_sequence)
} else {
    Write-Error ("Agent control rejected: {0}: {1}" -f $result.error_code, $result.detail)
    exit 1
}
