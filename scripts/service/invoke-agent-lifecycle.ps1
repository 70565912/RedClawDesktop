param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('status', 'authorize', 'execute')]
    [string]$Operation,

    [ValidateSet('publish', 'stop', 'restart')]
    [string]$Action = 'stop',

    [string]$RequestId = '',

    [string]$ApprovalId = '',

    [string]$ExpectedGitSha = '',

    [string]$CandidateExeSha256 = '',

    [string]$TargetPathSha256 = '',

    [string]$TargetPath = '',

    [long]$TargetPid = 0,

    [switch]$BuildGatePassed,

    [switch]$FocusedTestGatePassed,

    [switch]$OperatorApproved,

    [ValidateRange(1, 120)]
    [int]$ApprovalTtlSeconds = 120,

    [string]$ControlName = 'RedClawDesktop.AgentLifecycle.v1',

    [ValidateRange(100, 30000)]
    [int]$TimeoutMs = 5000,

    [switch]$Json
)

$ErrorActionPreference = 'Stop'

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

if (-not [string]::IsNullOrWhiteSpace($TargetPath)) {
    $normalizedTargetPath = [System.IO.Path]::GetFullPath($TargetPath).TrimEnd('\').ToLowerInvariant()
    $computedTargetPathSha256 = Get-StringSha256 -Value $normalizedTargetPath
    if (-not [string]::IsNullOrWhiteSpace($TargetPathSha256) -and
        $TargetPathSha256.ToLowerInvariant() -ne $computedTargetPathSha256) {
        throw 'TargetPath and TargetPathSha256 identify different targets.'
    }
    $TargetPathSha256 = $computedTargetPathSha256
}

if ([string]::IsNullOrWhiteSpace($RequestId)) {
    $RequestId = 'lifecycle-' + [guid]::NewGuid().ToString('N')
}
if ($Operation -ne 'status') {
    if ($ExpectedGitSha -notmatch '^[0-9a-fA-F]{40,64}$' -or
        $CandidateExeSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        $TargetPathSha256 -notmatch '^[0-9a-fA-F]{64}$' -or $TargetPid -le 0) {
        throw 'authorize/execute requires exact Git SHA, candidate EXE SHA256, target path SHA256, and target PID.'
    }
    if (-not $BuildGatePassed -or -not $FocusedTestGatePassed) {
        throw 'authorize/execute requires both build and focused-test gates.'
    }
}
if ($Operation -eq 'authorize' -and -not $OperatorApproved) {
    throw 'authorize requires -OperatorApproved.'
}
if ($Operation -eq 'execute' -and [string]::IsNullOrWhiteSpace($ApprovalId)) {
    throw 'execute requires the short-lived -ApprovalId returned by authorize.'
}

$request = [ordered]@{
    schema = 'redclaw.agent-lifecycle.v1'
    request_id = $RequestId
    operation = $Operation
}
if ($Operation -ne 'status') {
    $request.action = $Action
    $request.expected_git_sha = $ExpectedGitSha.ToLowerInvariant()
    $request.candidate_exe_sha256 = $CandidateExeSha256.ToLowerInvariant()
    $request.target_path_sha256 = $TargetPathSha256.ToLowerInvariant()
    $request.target_pid = $TargetPid
    $request.build_gate_passed = [bool]$BuildGatePassed
    $request.focused_test_gate_passed = [bool]$FocusedTestGatePassed
}
if ($Operation -eq 'authorize') {
    $request.operator_approved = [bool]$OperatorApproved
    $request.approval_ttl_seconds = $ApprovalTtlSeconds
}
if ($Operation -eq 'execute') {
    $request.approval_id = $ApprovalId
}

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
        $writer.WriteLine(($request | ConvertTo-Json -Depth 6 -Compress))
        $readTask = $reader.ReadLineAsync()
        if (-not $readTask.Wait($TimeoutMs)) {
            throw "Agent lifecycle response timed out after $TimeoutMs ms."
        }
        $line = $readTask.GetAwaiter().GetResult()
        if ([string]::IsNullOrWhiteSpace($line)) {
            throw 'Agent lifecycle supervisor returned an empty response.'
        }
        $response = $line | ConvertFrom-Json
    } finally {
        $reader.Dispose()
        $writer.Dispose()
    }
} finally {
    $pipe.Dispose()
}

if ($Json) {
    $response | ConvertTo-Json -Depth 8
} elseif ([bool]$response.ok) {
    Write-Host ("[agent-lifecycle] operation={0} phase={1} runtime_pid={2} exe_sha256={3}" -f `
        $Operation, $response.status.phase, $response.status.runtime_pid,
        $response.status.runtime_exe_sha256)
    if ($Operation -eq 'authorize') {
        Write-Host ("[agent-lifecycle] approval_id={0} expires_at_ms={1}" -f `
            $response.result.approval_id, $response.result.expires_at_ms)
    }
} else {
    Write-Error ("Agent lifecycle rejected: {0}: {1}" -f `
        $response.error_code, $response.error_detail)
    exit 1
}
