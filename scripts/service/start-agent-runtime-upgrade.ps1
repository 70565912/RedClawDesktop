param(
    [Parameter(Mandatory)][int]$TargetPid,
    [Parameter(Mandatory)][string]$CandidateDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$ExpectedGitSha,
    [Parameter(Mandatory)][string]$EvidenceDirectory,
    [switch]$BuildGatePassed,
    [switch]$FocusedTestGatePassed,
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
if (-not $BuildGatePassed -or -not $FocusedTestGatePassed) { throw 'upgrade_prerequisite_gate_failed' }
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if ((& git -C $repoRoot rev-parse HEAD).Trim() -ne $ExpectedGitSha) { throw 'upgrade_repository_commit_mismatch' }
$identity = Get-UpgradeIdentity $TargetPid
$runtimeIdentities = @(Get-UpgradeRuntimeFamily $identity)
if ([IO.Path]::GetFileName($identity.path) -ne 'redclaw_desktop.exe') { throw 'upgrade_target_is_not_redclaw' }
if ($identity.session_id -ne (Get-Process -Id $PID).SessionId) { throw 'upgrade_interactive_session_mismatch' }
$formal = Get-UpgradeDirectory (Split-Path $identity.path)
$candidate = Get-UpgradeDirectory $CandidateDirectory
if ($formal -eq $candidate -or $candidate.StartsWith($formal + '\',[StringComparison]::OrdinalIgnoreCase) -or $formal.StartsWith($candidate + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'upgrade_directories_overlap' }
$manifest = @(Get-UpgradeManifest $candidate)
Assert-UpgradeManifest $candidate $manifest
$arguments = @(ConvertFrom-UpgradeCommandLine $identity.command_line)
if ($arguments.Count -lt 2) { throw 'upgrade_gui_launch_identity_missing' }
$arguments = @($arguments | Select-Object -Skip 1)
$roleIndex = [Array]::IndexOf($arguments,'--gui-role')
if ($roleIndex -lt 0 -or $arguments[$roleIndex + 1] -notin @('host','controller')) { throw 'upgrade_gui_role_missing' }
$role = $arguments[$roleIndex + 1]
$pipeIndex = [Array]::IndexOf($arguments,'--debug-control-name')
$controlName = if ($pipeIndex -ge 0) {$arguments[$pipeIndex + 1]} else {''}
$updatedArguments = @($arguments)
$shaIndex = [Array]::IndexOf($updatedArguments,'--coordination-git-sha')
if ($shaIndex -ge 0) {$updatedArguments[$shaIndex + 1] = $ExpectedGitSha}
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$evidence = Get-UpgradeDirectory $EvidenceDirectory
if ($evidence.StartsWith($formal + '\',[StringComparison]::OrdinalIgnoreCase) -or $evidence.StartsWith($candidate + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'upgrade_evidence_must_be_independent' }
$operationId = [guid]::NewGuid().ToString('N')
$operationDirectory = Join-Path $evidence $operationId
New-Item -ItemType Directory -Path $operationDirectory | Out-Null
Protect-UpgradePath $operationDirectory
$planPath = Join-Path $operationDirectory 'plan.json'
$statusPath = Join-Path $operationDirectory 'status.json'
$taskName = 'RedClaw-Upgrade-' + $operationId
$plan = [ordered]@{
    schema='redclaw.runtime-upgrade.plan.v1'; operation_id=$operationId; task_name=$taskName;
    expected_git_sha=$ExpectedGitSha; repo_root=$repoRoot; target=$identity; runtime_identities=$runtimeIdentities; formal_directory=$formal; candidate_directory=$candidate;
    candidate_manifest=$manifest; original_manifest=@(Get-UpgradeManifest $formal);
    original_arguments=$arguments; updated_arguments=$updatedArguments; role=$role; control_name=$controlName;
    status_path=$statusPath; acknowledgment_path=(Join-Path $operationDirectory 'handoff-ack.txt');
    rollback_directory=($formal + '.rollback.' + $operationId); pending_directory=($formal + '.candidate.' + $operationId)
}
$plan | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $planPath -Encoding UTF8
Protect-UpgradePath $planPath
$planHash = (Get-FileHash -LiteralPath $planPath -Algorithm SHA256).Hash
if ($PlanOnly) {
    [pscustomobject]@{phase='validated';plan_path=$planPath;plan_sha256=$planHash;target_pid=$TargetPid;candidate_files=$manifest.Count} | ConvertTo-Json
    return
}
# Task Scheduler starts the fixed worker under this interactive user. Provider
# child-process jobs cannot kill it when the Host/Agent exits. No elevated token.
$worker = Join-Path $PSScriptRoot 'start-agent-lifecycle-supervisor.ps1'
$workerArguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$worker,'-UpgradePlanPath',$planPath,'-UpgradePlanSha256',$planHash)
$action = New-ScheduledTaskAction -Execute (Join-Path $PSHOME 'powershell.exe') -Argument (($workerArguments | ForEach-Object {ConvertTo-UpgradeArgument $_}) -join ' ')
if (-not (Test-Path -LiteralPath $action.Execute)) {
    $action = New-ScheduledTaskAction -Execute (Join-Path $PSHOME 'pwsh.exe') -Argument (($workerArguments | ForEach-Object {ConvertTo-UpgradeArgument $_}) -join ' ')
}
$principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $taskName
$deadline = [DateTimeOffset]::UtcNow.AddSeconds(40)
do {
    if (Test-Path -LiteralPath $statusPath) {
        try { $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json } catch { $status = $null }
        if ($status -and $status.phase -eq 'handoff_ready') {
            Assert-UpgradeIdentity $identity
            $workerProcess = Get-CimInstance Win32_Process -Filter "ProcessId=$($status.worker_pid)"
            if (-not $workerProcess -or $workerProcess.SessionId -ne $identity.session_id -or $workerProcess.ParentProcessId -eq $PID) { throw 'upgrade_worker_not_independent' }
            $planHash | Set-Content -LiteralPath $plan.acknowledgment_path -Encoding ASCII
            [pscustomobject]@{phase='independent_worker_owns_upgrade';worker_pid=$status.worker_pid;status_path=$statusPath;plan_path=$planPath;plan_sha256=$planHash} | ConvertTo-Json
            return
        }
        if ($status -and $status.phase -eq 'failed') { throw ('upgrade_worker_preflight_failed: ' + $status.detail) }
    }
    Start-Sleep -Milliseconds 200
} while ([DateTimeOffset]::UtcNow -lt $deadline)
throw 'upgrade_worker_handoff_timeout_target_not_stopped'
