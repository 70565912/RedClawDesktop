param(
    [string]$FormalRuntimePath = '',

    [string]$UpgradePlanPath = '',

    [string]$UpgradePlanSha256 = '',

    [string]$CandidateRuntimePath = '',

    [string]$RepoRoot = '',

    [string[]]$RuntimeArgument = @(),

    [string]$ControlName = 'RedClawDesktop.AgentLifecycle.v1',

    [string]$JournalPath = '',

    [ValidateRange(1, 120)]
    [int]$MaximumApprovalTtlSeconds = 120,

    [switch]$StartRuntime,

    [switch]$HiddenRuntime
)

$ErrorActionPreference = 'Stop'
if (-not [string]::IsNullOrWhiteSpace($UpgradePlanPath)) {
    & (Join-Path $PSScriptRoot 'invoke-runtime-directory-upgrade.ps1') -PlanPath $UpgradePlanPath -PlanSha256 $UpgradePlanSha256
    return
}
if ([string]::IsNullOrWhiteSpace($FormalRuntimePath)) { throw 'FormalRuntimePath or UpgradePlanPath is required.' }
$script:LifecycleSchema = 'redclaw.agent-lifecycle.v1'
$script:JournalSchema = 'redclaw.coordination.journal.v1'
$script:Approvals = @{}
$script:LifecycleRequestStates = @{}
$script:JournalSequence = 0
$script:RuntimeProcess = $null
$script:LastRuntimePid = 0
$script:Phase = 'idle'
$script:ChannelState = 'unavailable'

function Resolve-RequiredPath {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Label)

    try {
        return (Resolve-Path -LiteralPath $Path).Path
    } catch {
        throw "$Label was not found: $Path"
    }
}

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
    if (-not $acl.AreAccessRulesProtected) {
        throw 'Lifecycle journal ACL inherits permissions; refusing to append.'
    }
    $ownerSid = $acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value
    if ($ownerSid -ne $identity.User.Value) {
        throw 'Lifecycle journal is not owned by the current user; refusing to append.'
    }
    $currentUserAllowed = $false
    foreach ($rule in $acl.Access) {
        if ($rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow) {
            continue
        }
        $ruleSid = $rule.IdentityReference.Translate(
            [System.Security.Principal.SecurityIdentifier]).Value
        if ($ruleSid -ne $identity.User.Value) {
            throw 'Lifecycle journal grants another principal access; refusing to append.'
        }
        $currentUserAllowed = $true
    }
    if (-not $currentUserAllowed) {
        throw 'Lifecycle journal does not grant current-user access; refusing to append.'
    }
}

function ConvertTo-CommandLineArgument {
    param([AllowEmptyString()][string]$Value)

    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return ('"{0}"' -f ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1'))
}

function Get-StringSha256 {
    param([Parameter(Mandatory = $true)][string]$Value)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
            $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Value)))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Get-PathSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $normalized = [System.IO.Path]::GetFullPath($Path).TrimEnd('\').ToLowerInvariant()
    return Get-StringSha256 -Value $normalized
}

function Get-RequestDigest {
    param([Parameter(Mandatory = $true)]$Request)

    $material = @(
        'redclaw-agent-lifecycle-v1',
        [string]$Request.request_id,
        [string]$Request.action,
        [string]$Request.expected_git_sha,
        [string]$Request.candidate_exe_sha256,
        [string]$Request.target_path_sha256,
        [string][long]$Request.target_pid,
        ([string][bool]$Request.build_gate_passed).ToLowerInvariant(),
        ([string][bool]$Request.focused_test_gate_passed).ToLowerInvariant()
    ) -join "`n"
    return Get-StringSha256 -Value ($material + "`n")
}

function Get-CurrentGitSha {
    $value = (& git -C $script:RepoRootPath rev-parse HEAD 2>$null).Trim().ToLowerInvariant()
    if ($LASTEXITCODE -ne 0 -or $value -notmatch '^[0-9a-f]{40,64}$') {
        throw 'Unable to resolve the local Git SHA without network access.'
    }
    return $value
}

function Get-RuntimeProcess {
    if ($null -eq $script:RuntimeProcess) {
        return $null
    }
    try {
        $script:RuntimeProcess.Refresh()
        if ($script:RuntimeProcess.HasExited) {
            $script:LastRuntimePid = $script:RuntimeProcess.Id
            $script:RuntimeProcess = $null
            if ($script:Phase -notin @('stopped', 'published')) {
                $script:Phase = 'exited'
                $script:ChannelState = 'unavailable'
            }
            return $null
        }
        return $script:RuntimeProcess
    } catch {
        $script:RuntimeProcess = $null
        return $null
    }
}

function Get-ObservedIdentity {
    $process = Get-RuntimeProcess
    $observedPid = if ($null -ne $process) { [long]$process.Id } else { [long]$script:LastRuntimePid }
    return [pscustomobject]@{
        git_sha = Get-CurrentGitSha
        candidate_exe_sha256 = (Get-FileHash -LiteralPath $script:CandidateRuntime -Algorithm SHA256).Hash.ToLowerInvariant()
        target_path_sha256 = Get-PathSha256 -Path $script:FormalRuntime
        target_pid = $observedPid
        runtime_running = $null -ne $process
    }
}

function Test-RequestShape {
    param([Parameter(Mandatory = $true)]$Request)

    if ([string]$Request.schema -ne $script:LifecycleSchema) {
        throw 'invalid_schema'
    }
    if ([string]$Request.request_id -notmatch '^[A-Za-z0-9._/-]{1,128}$') {
        throw 'invalid_request_id'
    }
    if ([string]$Request.action -notin @('publish', 'stop', 'restart')) {
        throw 'invalid_action'
    }
    if ([string]$Request.expected_git_sha -notmatch '^[0-9a-fA-F]{40,64}$' -or
        [string]$Request.candidate_exe_sha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        [string]$Request.target_path_sha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        [long]$Request.target_pid -le 0) {
        throw 'invalid_identity'
    }
    if (-not [bool]$Request.build_gate_passed -or
        -not [bool]$Request.focused_test_gate_passed) {
        throw 'prerequisite_gate_failed'
    }
}

function Assert-ObservedIdentity {
    param([Parameter(Mandatory = $true)]$Request)

    $observed = Get-ObservedIdentity
    if ([string]$Request.expected_git_sha -ne [string]$observed.git_sha -or
        [string]$Request.candidate_exe_sha256 -ne [string]$observed.candidate_exe_sha256 -or
        [string]$Request.target_path_sha256 -ne [string]$observed.target_path_sha256 -or
        [long]$Request.target_pid -ne [long]$observed.target_pid) {
        throw 'stale_pid_path_or_hash'
    }
    return $observed
}

function Append-JournalRecord {
    param(
        [Parameter(Mandatory = $true)]$Request,
        [Parameter(Mandatory = $true)][string]$AuthorizationState,
        [Parameter(Mandatory = $true)][string]$TerminalResult
    )

    Assert-OwnerOnlyFile -Path $script:JournalFile
    $script:JournalSequence++
    $record = [ordered]@{
        schema = $script:JournalSchema
        sequence = $script:JournalSequence
        recorded_at_ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        record_type = 'lifecycle'
        authority = 'normal_agent'
        request_state = $(if ($TerminalResult -eq 'completed') { 'completed' } elseif ($TerminalResult -eq 'authorized') { 'accepted' } else { 'failed' })
        request_id = [string]$Request.request_id
        reply_to_request_id = ''
        supersedes_request_id = ''
        task_id = ''
        request_digest_sha256 = Get-RequestDigest -Request $Request
        authorization_state = $AuthorizationState
        git_sha = [string]$Request.expected_git_sha
        executable_sha256 = [string]$Request.candidate_exe_sha256
        terminal_result = $TerminalResult
        evidence_manifest_name = ''
        evidence_sha256 = ''
        acknowledged_event_sequence = 0
    }
    $json = $record | ConvertTo-Json -Compress
    $writer = [System.IO.StreamWriter]::new(
        $script:JournalFile, $true, [System.Text.UTF8Encoding]::new($false))
    try {
        $writer.WriteLine($json)
        $writer.Flush()
    } finally {
        $writer.Dispose()
    }
    if ($AuthorizationState -in @('granted', 'consumed')) {
        $script:LifecycleRequestStates[[string]$Request.request_id] = $TerminalResult
    }
}

function Start-FormalRuntime {
    $argumentLine = ($RuntimeArgument | ForEach-Object {
        ConvertTo-CommandLineArgument -Value $_
    }) -join ' '
    $start = @{
        FilePath = $script:FormalRuntime
        PassThru = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($argumentLine)) {
        $start.ArgumentList = $argumentLine
    }
    if ($HiddenRuntime) {
        $start.WindowStyle = 'Hidden'
    }
    $script:RuntimeProcess = Start-Process @start
    $script:LastRuntimePid = $script:RuntimeProcess.Id
    $script:Phase = 'starting'
    $script:ChannelState = 'reconnecting'
    return $script:RuntimeProcess
}

function Stop-FormalRuntime {
    param([Parameter(Mandatory = $true)][long]$ExpectedPid)

    $process = Get-RuntimeProcess
    if ($null -eq $process -or $process.Id -ne $ExpectedPid) {
        throw 'target_process_not_running'
    }
    try {
        $actualPath = $process.MainModule.FileName
    } catch {
        throw 'target_process_path_unavailable'
    }
    if ([string]::IsNullOrWhiteSpace($actualPath) -or
        [System.IO.Path]::GetFullPath($actualPath) -ne [System.IO.Path]::GetFullPath($script:FormalRuntime)) {
        throw 'target_process_path_mismatch'
    }
    Stop-Process -Id $ExpectedPid
    if (-not $process.WaitForExit(10000)) {
        throw 'target_process_stop_timeout'
    }
    $script:LastRuntimePid = $ExpectedPid
    $script:RuntimeProcess = $null
    $script:Phase = 'stopped'
    $script:ChannelState = 'unavailable'
}

function Publish-FormalRuntime {
    param([Parameter(Mandatory = $true)][string]$ExpectedHash)

    if ($null -ne (Get-RuntimeProcess)) {
        throw 'publish_requires_stopped_runtime'
    }
    if ([System.IO.Path]::GetFullPath($script:CandidateRuntime) -ne
        [System.IO.Path]::GetFullPath($script:FormalRuntime)) {
        $pending = $script:FormalRuntime + '.pending.' + [guid]::NewGuid().ToString('N')
        Copy-Item -LiteralPath $script:CandidateRuntime -Destination $pending
        try {
            $pendingHash = (Get-FileHash -LiteralPath $pending -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($pendingHash -ne $ExpectedHash) {
                throw 'staged_candidate_hash_mismatch'
            }
            Move-Item -Force -LiteralPath $pending -Destination $script:FormalRuntime
        } finally {
            if (Test-Path -LiteralPath $pending) {
                Remove-Item -Force -LiteralPath $pending
            }
        }
    }
    $publishedHash = (Get-FileHash -LiteralPath $script:FormalRuntime -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($publishedHash -ne $ExpectedHash) {
        throw 'published_executable_hash_mismatch'
    }
    $script:Phase = 'published'
    $script:ChannelState = 'unavailable'
}

function New-PipeSecurity {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $security = [System.IO.Pipes.PipeSecurity]::new()
    $security.SetOwner($identity.User)
    $security.SetAccessRuleProtection($true, $false)
    $rule = [System.IO.Pipes.PipeAccessRule]::new(
        $identity.User,
        [System.IO.Pipes.PipeAccessRights]::ReadWrite,
        [System.Security.AccessControl.AccessControlType]::Allow)
    $security.AddAccessRule($rule)
    return $security
}

function New-OwnerOnlyPipe {
    $optionNames = [enum]::GetNames([System.IO.Pipes.PipeOptions])
    if ($optionNames -contains 'CurrentUserOnly') {
        $options = [System.IO.Pipes.PipeOptions]::CurrentUserOnly
        return [System.IO.Pipes.NamedPipeServerStream]::new(
            $ControlName,
            [System.IO.Pipes.PipeDirection]::InOut,
            1,
            [System.IO.Pipes.PipeTransmissionMode]::Byte,
            $options,
            65536,
            65536)
    }
    return [System.IO.Pipes.NamedPipeServerStream]::new(
        $ControlName,
        [System.IO.Pipes.PipeDirection]::InOut,
        1,
        [System.IO.Pipes.PipeTransmissionMode]::Byte,
        [System.IO.Pipes.PipeOptions]::None,
        65536,
        65536,
        (New-PipeSecurity))
}

function New-Response {
    param(
        [string]$RequestId,
        [bool]$Ok,
        [string]$ErrorCode = '',
        [string]$ErrorDetail = '',
        $Result = $null
    )

    $process = Get-RuntimeProcess
    $runtimeHash = if (Test-Path -LiteralPath $script:FormalRuntime) {
        (Get-FileHash -LiteralPath $script:FormalRuntime -Algorithm SHA256).Hash.ToLowerInvariant()
    } else { '' }
    return [ordered]@{
        schema = $script:LifecycleSchema
        request_id = $RequestId
        ok = $Ok
        error_code = $ErrorCode
        error_detail = $(if ($ErrorDetail.Length -gt 512) { $ErrorDetail.Substring(0, 512) } else { $ErrorDetail })
        result = $Result
        status = [ordered]@{
            supervisor_pid = $PID
            runtime_pid = $(if ($null -ne $process) { $process.Id } else { 0 })
            runtime_exe_sha256 = $runtimeHash
            phase = $script:Phase
            channel_state = $script:ChannelState
        }
    }
}

$script:FormalRuntime = Resolve-RequiredPath -Path $FormalRuntimePath -Label 'Formal runtime'
if ([string]::IsNullOrWhiteSpace($CandidateRuntimePath)) {
    $CandidateRuntimePath = $script:FormalRuntime
}
$script:CandidateRuntime = Resolve-RequiredPath -Path $CandidateRuntimePath -Label 'Candidate runtime'
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$script:RepoRootPath = Resolve-RequiredPath -Path $RepoRoot -Label 'Repository root'
if ([string]::IsNullOrWhiteSpace($JournalPath)) {
    $JournalPath = Join-Path $env:LOCALAPPDATA 'RedClawDesktop\coordination\lifecycle-journal-v1.jsonl'
}
$journalDirectory = Split-Path -Parent $JournalPath
New-Item -ItemType Directory -Force -Path $journalDirectory | Out-Null
$script:JournalFile = [System.IO.Path]::GetFullPath($JournalPath)
if (Test-Path -LiteralPath $script:JournalFile) {
    Assert-OwnerOnlyFile -Path $script:JournalFile
    $expectedSequence = 1L
    foreach ($journalLine in Get-Content -LiteralPath $script:JournalFile) {
        if ([string]::IsNullOrWhiteSpace($journalLine)) {
            throw 'Existing lifecycle journal contains a blank record; refusing to append.'
        }
        try {
            $record = $journalLine | ConvertFrom-Json
        } catch {
            throw 'Existing lifecycle journal is invalid; refusing to truncate or replace it.'
        }
        if ([string]$record.schema -ne $script:JournalSchema -or
            [string]$record.record_type -ne 'lifecycle' -or
            [long]$record.sequence -ne $expectedSequence) {
            throw 'Existing lifecycle journal schema or sequence is invalid; refusing to append.'
        }
        if ([string]$record.authorization_state -in @('granted', 'consumed')) {
            $script:LifecycleRequestStates[[string]$record.request_id] = [string]$record.terminal_result
        }
        $script:JournalSequence = [long]$record.sequence
        $expectedSequence++
    }
} else {
    [System.IO.File]::WriteAllText(
        $script:JournalFile, '', [System.Text.UTF8Encoding]::new($false))
    Protect-OwnerOnlyFile -Path $script:JournalFile
}

if ($StartRuntime) {
    [void](Start-FormalRuntime)
}

Write-Host ("[agent-lifecycle] ready pipe={0} supervisor_pid={1} runtime_pid={2}" -f `
    $ControlName, $PID, $(if ($null -ne (Get-RuntimeProcess)) { $script:RuntimeProcess.Id } else { 0 }))

while ($true) {
    $pipe = New-OwnerOnlyPipe
    try {
        $pipe.WaitForConnection()
        $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
        $reader = [System.IO.StreamReader]::new($pipe, $utf8NoBom, $false, 4096, $true)
        $writer = [System.IO.StreamWriter]::new($pipe, $utf8NoBom, 4096, $true)
        try {
            $writer.AutoFlush = $true
            $line = $reader.ReadLine()
            if ([string]::IsNullOrWhiteSpace($line) -or
                [System.Text.Encoding]::UTF8.GetByteCount($line) -gt 65536) {
                $response = New-Response -RequestId 'unknown' -Ok $false `
                    -ErrorCode 'invalid_request' -ErrorDetail 'Lifecycle request is empty or oversized.'
            } else {
                $request = $null
                $requestId = 'unknown'
                try {
                    try {
                        $request = $line | ConvertFrom-Json
                    } catch {
                        throw 'invalid_json'
                    }
                    $requestId = [string]$request.request_id
                    if ([string]$request.schema -ne $script:LifecycleSchema) {
                        throw 'invalid_schema'
                    }
                    switch ([string]$request.operation) {
                        'status' {
                            $response = New-Response -RequestId $requestId -Ok $true
                        }
                        'authorize' {
                            Test-RequestShape -Request $request
                            if ($script:LifecycleRequestStates.ContainsKey([string]$request.request_id)) {
                                throw 'duplicate_lifecycle_request'
                            }
                            [void](Assert-ObservedIdentity -Request $request)
                            if (-not [bool]$request.operator_approved) {
                                throw 'explicit_operator_approval_required'
                            }
                            $ttl = [int]$request.approval_ttl_seconds
                            if ($ttl -lt 1 -or $ttl -gt $MaximumApprovalTtlSeconds) {
                                throw 'invalid_approval_ttl'
                            }
                            $approvalId = 'approval-' + [guid]::NewGuid().ToString('N')
                            $approval = [pscustomobject]@{
                                request_digest = Get-RequestDigest -Request $request
                                expires_at_ms = [DateTimeOffset]::UtcNow.AddSeconds($ttl).ToUnixTimeMilliseconds()
                                consumed = $false
                            }
                            $script:Approvals[$approvalId] = $approval
                            Append-JournalRecord -Request $request `
                                -AuthorizationState 'granted' -TerminalResult 'authorized'
                            $response = New-Response -RequestId $requestId -Ok $true -Result ([ordered]@{
                                approval_id = $approvalId
                                request_digest_sha256 = $approval.request_digest
                                expires_at_ms = $approval.expires_at_ms
                            })
                        }
                        'execute' {
                            Test-RequestShape -Request $request
                            if ($script:LifecycleRequestStates[[string]$request.request_id] -ne 'authorized') {
                                throw 'lifecycle_request_not_authorized'
                            }
                            $approvalId = [string]$request.approval_id
                            $approval = $script:Approvals[$approvalId]
                            if ($null -eq $approval -or [bool]$approval.consumed) {
                                throw 'approval_missing_or_consumed'
                            }
                            if ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() -gt [long]$approval.expires_at_ms) {
                                throw 'approval_expired'
                            }
                            if ([string]$approval.request_digest -ne (Get-RequestDigest -Request $request)) {
                                throw 'request_changed_after_approval'
                            }
                            [void](Assert-ObservedIdentity -Request $request)
                            $approval.consumed = $true
                            switch ([string]$request.action) {
                                'stop' { Stop-FormalRuntime -ExpectedPid ([long]$request.target_pid) }
                                'publish' { Publish-FormalRuntime -ExpectedHash ([string]$request.candidate_exe_sha256) }
                                'restart' {
                                    if ($null -ne (Get-RuntimeProcess)) {
                                        throw 'restart_requires_stopped_runtime'
                                    }
                                    $formalHash = (Get-FileHash -LiteralPath $script:FormalRuntime -Algorithm SHA256).Hash.ToLowerInvariant()
                                    if ($formalHash -ne [string]$request.candidate_exe_sha256) {
                                        throw 'formal_executable_hash_mismatch'
                                    }
                                    [void](Start-FormalRuntime)
                                }
                            }
                            Append-JournalRecord -Request $request `
                                -AuthorizationState 'consumed' -TerminalResult 'completed'
                            $runtime = Get-RuntimeProcess
                            $response = New-Response -RequestId $requestId -Ok $true -Result ([ordered]@{
                                action = [string]$request.action
                                new_pid = $(if ($null -ne $runtime) { $runtime.Id } else { 0 })
                                exe_sha256 = (Get-FileHash -LiteralPath $script:FormalRuntime -Algorithm SHA256).Hash.ToLowerInvariant()
                                phase = $script:Phase
                                channel_state = $script:ChannelState
                            })
                        }
                        default { throw 'invalid_operation' }
                    }
                } catch {
                    $errorCode = $_.Exception.Message
                    if ($null -ne $request -and [string]$request.action -in @('publish', 'stop', 'restart') -and
                        [string]$request.request_id -match '^[A-Za-z0-9._/-]{1,128}$' -and
                        [string]$request.expected_git_sha -match '^[0-9a-fA-F]{40,64}$' -and
                        [string]$request.candidate_exe_sha256 -match '^[0-9a-fA-F]{64}$' -and
                        [string]$request.target_path_sha256 -match '^[0-9a-fA-F]{64}$' -and
                        [long]$request.target_pid -gt 0) {
                        try {
                            Append-JournalRecord -Request $request `
                                -AuthorizationState 'rejected' -TerminalResult 'failed'
                        } catch {
                            $errorCode = 'journal_append_failed'
                        }
                    }
                    $response = New-Response -RequestId $requestId -Ok $false `
                        -ErrorCode $errorCode -ErrorDetail $errorCode
                }
            }
            $writer.WriteLine(($response | ConvertTo-Json -Depth 8 -Compress))
        } finally {
            $writer.Dispose()
            $reader.Dispose()
        }
    } finally {
        $pipe.Dispose()
    }
}
