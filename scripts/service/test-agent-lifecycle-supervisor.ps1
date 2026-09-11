param()

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$supervisorScript = Join-Path $PSScriptRoot 'start-agent-lifecycle-supervisor.ps1'
$lifecycleClient = Join-Path $PSScriptRoot 'invoke-agent-lifecycle.ps1'
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'RedClawAgentLifecycleTest-' + [guid]::NewGuid().ToString('N'))
$formalRuntime = Join-Path $testRoot 'formal-runtime.exe'
$candidateRuntime = Join-Path $testRoot 'candidate-runtime.exe'
$journalPath = Join-Path $testRoot 'lifecycle-journal-v1.jsonl'
$stdoutPath = Join-Path $testRoot 'supervisor.stdout.log'
$stderrPath = Join-Path $testRoot 'supervisor.stderr.log'
$controlName = 'RedClawDesktop.AgentLifecycle.Test.' + [guid]::NewGuid().ToString('N')
$supervisor = $null
$lastRuntimePid = 0

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

function Send-LifecycleRequest {
    param([Parameter(Mandatory = $true)]$Request, [int]$TimeoutMs = 5000)

    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.', $controlName, [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    try {
        $pipe.Connect($TimeoutMs)
        $encoding = [System.Text.UTF8Encoding]::new($false)
        $writer = [System.IO.StreamWriter]::new($pipe, $encoding, 4096, $true)
        $reader = [System.IO.StreamReader]::new($pipe, $encoding, $false, 4096, $true)
        try {
            $writer.AutoFlush = $true
            $line = if ($Request -is [string]) {
                [string]$Request
            } else {
                $Request | ConvertTo-Json -Depth 8 -Compress
            }
            $writer.WriteLine($line)
            $readTask = $reader.ReadLineAsync()
            if (-not $readTask.Wait($TimeoutMs)) {
                throw 'Lifecycle supervisor response timed out.'
            }
            return ($readTask.GetAwaiter().GetResult() | ConvertFrom-Json)
        } finally {
            $reader.Dispose()
            $writer.Dispose()
        }
    } finally {
        $pipe.Dispose()
    }
}

function New-LifecycleRequest {
    param(
        [Parameter(Mandatory = $true)][string]$RequestId,
        [Parameter(Mandatory = $true)][string]$Operation,
        [Parameter(Mandatory = $true)][string]$Action,
        [Parameter(Mandatory = $true)][long]$TargetPid,
        [string]$ApprovalId = '',
        [bool]$BuildGatePassed = $true,
        [bool]$FocusedTestGatePassed = $true
    )

    $request = [ordered]@{
        schema = 'redclaw.agent-lifecycle.v1'
        request_id = $RequestId
        operation = $Operation
        action = $Action
        expected_git_sha = $script:gitSha
        candidate_exe_sha256 = $script:candidateHash
        target_path_sha256 = $script:targetPathHash
        target_pid = $TargetPid
        build_gate_passed = $BuildGatePassed
        focused_test_gate_passed = $FocusedTestGatePassed
    }
    if ($Operation -eq 'authorize') {
        $request.operator_approved = $true
        $request.approval_ttl_seconds = 30
    } elseif ($Operation -eq 'execute') {
        $request.approval_id = $ApprovalId
    }
    return $request
}

function Assert-True {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)

    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-ApprovedAction {
    param(
        [Parameter(Mandatory = $true)][string]$Action,
        [Parameter(Mandatory = $true)][long]$TargetPid
    )

    $requestId = 'local-gate-' + $Action + '-' + [guid]::NewGuid().ToString('N')
    $authorization = (& $lifecycleClient -Operation authorize -Action $Action `
        -RequestId $requestId -ExpectedGitSha $script:gitSha `
        -CandidateExeSha256 $script:candidateHash -TargetPath $formalRuntime `
        -TargetPid $TargetPid -BuildGatePassed -FocusedTestGatePassed `
        -OperatorApproved -ApprovalTtlSeconds 30 -ControlName $controlName -Json |
        Out-String | ConvertFrom-Json)
    Assert-True -Condition ([bool]$authorization.ok) `
        -Message "$Action authorization was rejected: $($authorization.error_code)"
    $result = (& $lifecycleClient -Operation execute -Action $Action `
        -RequestId $requestId -ApprovalId ([string]$authorization.result.approval_id) `
        -ExpectedGitSha $script:gitSha -CandidateExeSha256 $script:candidateHash `
        -TargetPath $formalRuntime -TargetPid $TargetPid `
        -BuildGatePassed -FocusedTestGatePassed -ControlName $controlName -Json |
        Out-String | ConvertFrom-Json)
    Assert-True -Condition ([bool]$result.ok) `
        -Message "$Action execution was rejected: $($result.error_code)"
    return $result
}

try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    $commandProcessor = Join-Path $env:SystemRoot 'System32\cmd.exe'
    Copy-Item -LiteralPath $commandProcessor -Destination $formalRuntime
    Copy-Item -LiteralPath $commandProcessor -Destination $candidateRuntime

    $script:gitSha = (& git -C $repoRoot rev-parse HEAD).Trim().ToLowerInvariant()
    Assert-True -Condition ($LASTEXITCODE -eq 0 -and $script:gitSha -match '^[0-9a-f]{40,64}$') `
        -Message 'Could not resolve the local Git SHA.'
    $script:candidateHash = (Get-FileHash -LiteralPath $candidateRuntime -Algorithm SHA256).Hash.ToLowerInvariant()
    $script:targetPathHash = Get-PathSha256 -Path $formalRuntime

    $shellPath = (Get-Process -Id $PID).MainModule.FileName
    $arguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $supervisorScript),
        '-FormalRuntimePath', ('"{0}"' -f $formalRuntime),
        '-CandidateRuntimePath', ('"{0}"' -f $candidateRuntime),
        '-RepoRoot', ('"{0}"' -f $repoRoot),
        '-ControlName', $controlName,
        '-JournalPath', ('"{0}"' -f $journalPath),
        '-StartRuntime',
        '-HiddenRuntime'
    )
    $supervisor = Start-Process -FilePath $shellPath -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath

    $status = $null
    $lastStatusError = ''
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        try {
            $status = Send-LifecycleRequest -Request ([ordered]@{
                schema = 'redclaw.agent-lifecycle.v1'
                request_id = 'local-gate-status'
                operation = 'status'
            }) -TimeoutMs 500
        } catch {
            $lastStatusError = $_.Exception.Message
            $status = $null
        }
    } while ($null -eq $status -and [DateTimeOffset]::UtcNow -lt $deadline)
    Assert-True -Condition ($null -ne $status -and [bool]$status.ok) `
        -Message "Lifecycle supervisor did not become ready: $lastStatusError"
    $runtimePid = [long]$status.status.runtime_pid
    $lastRuntimePid = $runtimePid
    Assert-True -Condition ($runtimePid -gt 0) -Message 'Disposable runtime was not started.'
    Assert-True -Condition ([long]$status.status.supervisor_pid -eq $supervisor.Id) `
        -Message 'Status was not returned by the expected supervisor.'

    $malformedResult = Send-LifecycleRequest -Request '{"broken"'
    Assert-True -Condition (-not [bool]$malformedResult.ok -and
        [string]$malformedResult.error_code -eq 'invalid_json' -and
        -not $supervisor.HasExited) `
        -Message 'Malformed JSON did not return a bounded error while preserving the supervisor.'

    $missingApproval = New-LifecycleRequest -RequestId 'local-gate-missing-approval' `
        -Operation execute -Action stop -TargetPid $runtimePid -ApprovalId 'missing'
    $missingApprovalResult = Send-LifecycleRequest -Request $missingApproval
    Assert-True -Condition (-not [bool]$missingApprovalResult.ok) `
        -Message 'Missing lifecycle approval was unexpectedly accepted.'

    $stalePid = New-LifecycleRequest -RequestId 'local-gate-stale-pid' `
        -Operation authorize -Action stop -TargetPid ($runtimePid + 1)
    $stalePidResult = Send-LifecycleRequest -Request $stalePid
    Assert-True -Condition (-not [bool]$stalePidResult.ok -and
        [string]$stalePidResult.error_code -eq 'stale_pid_path_or_hash') `
        -Message ("Stale PID did not fail closed: ok={0} error={1}" -f `
            $stalePidResult.ok, $stalePidResult.error_code)

    $failedGate = New-LifecycleRequest -RequestId 'local-gate-failed-test' `
        -Operation authorize -Action stop -TargetPid $runtimePid -FocusedTestGatePassed $false
    $failedGateResult = Send-LifecycleRequest -Request $failedGate
    Assert-True -Condition (-not [bool]$failedGateResult.ok -and
        [string]$failedGateResult.error_code -eq 'prerequisite_gate_failed') `
        -Message 'Failed focused-test prerequisite did not fail closed.'

    $stopResult = Invoke-ApprovedAction -Action stop -TargetPid $runtimePid
    Assert-True -Condition ([long]$stopResult.status.runtime_pid -eq 0) `
        -Message 'Disposable runtime remained active after the approved stop.'
    Assert-True -Condition (-not $supervisor.HasExited) `
        -Message 'Supervisor exited when the disposable runtime stopped.'

    $publishResult = Invoke-ApprovedAction -Action publish -TargetPid $runtimePid
    Assert-True -Condition ([string]$publishResult.result.exe_sha256 -eq $script:candidateHash) `
        -Message 'Published runtime hash does not match the approved candidate.'

    $restartResult = Invoke-ApprovedAction -Action restart -TargetPid $runtimePid
    $lastRuntimePid = [long]$restartResult.result.new_pid
    Assert-True -Condition ($lastRuntimePid -gt 0 -and $lastRuntimePid -ne $runtimePid) `
        -Message 'Approved restart did not return a new runtime PID.'
    Assert-True -Condition (-not $supervisor.HasExited) `
        -Message 'Supervisor did not survive the approved runtime replacement.'

    $duplicateAuthorization = New-LifecycleRequest `
        -RequestId ([string]$restartResult.request_id) -Operation authorize `
        -Action restart -TargetPid $lastRuntimePid
    $duplicateResult = Send-LifecycleRequest -Request $duplicateAuthorization
    Assert-True -Condition (-not [bool]$duplicateResult.ok -and
        [string]$duplicateResult.error_code -eq 'duplicate_lifecycle_request') `
        -Message 'A duplicate lifecycle request ID was not rejected.'

    $journalText = Get-Content -LiteralPath $journalPath -Raw
    Assert-True -Condition (-not $journalText.Contains($formalRuntime) -and
        -not $journalText.Contains($candidateRuntime)) `
        -Message 'Lifecycle journal persisted a raw local path.'
    $records = @(Get-Content -LiteralPath $journalPath | ForEach-Object { $_ | ConvertFrom-Json })
    for ($index = 0; $index -lt $records.Count; $index++) {
        Assert-True -Condition ([long]$records[$index].sequence -eq ($index + 1)) `
            -Message 'Lifecycle journal sequence is not contiguous.'
    }
    Assert-True -Condition (@($records | Where-Object { $_.terminal_result -eq 'completed' }).Count -eq 3) `
        -Message 'Lifecycle journal did not record exactly three completed actions.'

    Write-Host ('[agent-lifecycle-test] PASS supervisor_pid={0} old_runtime_pid={1} new_runtime_pid={2} records={3}' -f `
        $supervisor.Id, $runtimePid, $lastRuntimePid, $records.Count)
} catch {
    if (Test-Path -LiteralPath $stdoutPath) {
        Write-Host '[agent-lifecycle-test] supervisor stdout:'
        Get-Content -LiteralPath $stdoutPath | Write-Host
    }
    if (Test-Path -LiteralPath $stderrPath) {
        Write-Host '[agent-lifecycle-test] supervisor stderr:'
        Get-Content -LiteralPath $stderrPath | Write-Host
    }
    throw
} finally {
    if ($lastRuntimePid -gt 0) {
        Stop-Process -Id $lastRuntimePid -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $supervisor -and -not $supervisor.HasExited) {
        Stop-Process -Id $supervisor.Id -Force -ErrorAction SilentlyContinue
        [void]$supervisor.WaitForExit(5000)
    }
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = (Resolve-Path -LiteralPath $testRoot).Path
        $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
        if ($resolvedTestRoot.StartsWith($resolvedTempRoot + '\', [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedTestRoot) -like 'RedClawAgentLifecycleTest-*') {
            Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
        }
    }
}
