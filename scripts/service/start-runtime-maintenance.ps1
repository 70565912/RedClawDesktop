param(
    [Parameter(Mandatory)][string]$ContextPath,
    [Parameter(Mandatory)][ValidateSet('restart','update')][string]$Action,
    [string]$CandidateDirectory = '',
    [ValidatePattern('^[0-9a-f]{32}$')][string]$OperationId = ([guid]::NewGuid().ToString('N')),
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
Assert-UpgradeOwner $ContextPath
$context = Get-Content -LiteralPath $ContextPath -Raw | ConvertFrom-Json
if ($context.schema -ne 'redclaw.runtime-maintenance.context.v1' -or $context.role -ne 'host') { throw 'maintenance_context_invalid' }
$contextDirectory = Get-UpgradeDirectory (Split-Path ([IO.Path]::GetFullPath($ContextPath)))
Assert-UpgradeOwner $contextDirectory
$operationDirectory = Join-Path $contextDirectory $OperationId
$planPath = Join-Path $operationDirectory 'plan.json'
$statusPath = Join-Path $operationDirectory 'status.json'
$lock = [IO.File]::Open((Join-Path $contextDirectory 'maintenance.lock'),'OpenOrCreate','ReadWrite','None')
$createdPlan = $false
$registeredTask = $false
try {
    $receiptContexts = [ordered]@{}
    $ancestorPath = [IO.Path]::GetFullPath($ContextPath)
    $ancestor = $context
    while ($true) {
        Assert-UpgradeOwner $ancestorPath
        $ancestorDirectory = Get-UpgradeDirectory (Split-Path $ancestorPath)
        Assert-UpgradeOwner $ancestorDirectory
        if ($receiptContexts.Contains($ancestorDirectory) -or
            $ancestor.schema -ne 'redclaw.runtime-maintenance.context.v1' -or $ancestor.role -ne 'host' -or
            [IO.Path]::GetFullPath($ancestor.executable) -ne [IO.Path]::GetFullPath($context.executable)) {
            throw 'maintenance_context_history_invalid'
        }
        $receiptContexts[$ancestorDirectory] = $ancestorPath
        if ($ancestor.PSObject.Properties.Name -notcontains 'previous_context' -or -not $ancestor.previous_context) { break }
        $ancestorPath = [IO.Path]::GetFullPath($ancestor.previous_context)
        Assert-UpgradeOwner $ancestorPath
        $ancestor = Get-Content -LiteralPath $ancestorPath -Raw | ConvertFrom-Json
    }
    # A stable operation ID observes its existing receipt, even after Host exited.
    # Follow the immutable launch history before accepting a new operation.
    foreach ($receiptDirectory in $receiptContexts.Keys) {
        $existingDirectory = Join-Path $receiptDirectory $OperationId
        $existingPlanPath = Join-Path $existingDirectory 'plan.json'
        if (-not (Test-Path -LiteralPath $existingPlanPath)) { continue }
        Assert-UpgradeOwner $existingPlanPath
        $existing = Get-Content -LiteralPath $existingPlanPath -Raw | ConvertFrom-Json
        if ($existing.schema -ne 'redclaw.runtime-upgrade.plan.v2' -or $existing.action -ne $Action -or
            $existing.operation_id -ne $OperationId -or $existing.context_path -ne $receiptContexts[$receiptDirectory] -or
            ($Action -eq 'update' -and $existing.candidate_directory -ne [IO.Path]::GetFullPath($CandidateDirectory).TrimEnd('\'))) {
            throw 'maintenance_operation_id_conflict'
        }
        $phase = 'prepared'
        $existingStatusPath = Join-Path $existingDirectory 'status.json'
        if (Test-Path -LiteralPath $existingStatusPath) { $phase = (Get-UpgradeReceipt $existingStatusPath).phase }
        [pscustomobject]@{operation_id=$OperationId;phase=$phase;status_path=$existingStatusPath;replayed=$false} | ConvertTo-Json
        return
    }
    $activePath = Join-Path $contextDirectory 'active-operation.json'
    foreach ($receiptDirectory in $receiptContexts.Keys) {
        $previousActive = Join-Path $receiptDirectory 'active-operation.json'
        if (-not (Test-Path -LiteralPath $previousActive)) { continue }
        Assert-UpgradeOwner $previousActive
        $active = Get-Content -LiteralPath $previousActive -Raw | ConvertFrom-Json
        if ($active.schema -ne 'redclaw.runtime-maintenance.active.v1' -or $active.operation_id -notmatch '^[0-9a-f]{32}$') { throw 'maintenance_active_receipt_invalid' }
        $previousStatus = Join-Path (Join-Path $receiptDirectory $active.operation_id) 'status.json'
        if (-not (Test-Path -LiteralPath $previousStatus)) { throw 'maintenance_already_preparing' }
        $previous = Get-UpgradeReceipt $previousStatus
        if ($previous.phase -notin @('completed','failed','rolled_back','rollback_start_failed','restart_start_failed','interrupted')) {
            throw 'maintenance_already_active'
        }
    }
    $identity = Get-UpgradeIdentity ([int]$context.gui_pid)
    if ($identity.path -ne [IO.Path]::GetFullPath($context.executable) -or
        [DateTimeOffset]::Parse($identity.started).ToUnixTimeMilliseconds() -ne [long]$context.gui_started_ms -or
        [IO.Path]::GetFileName($identity.path) -ne 'redclaw_desktop.exe' -or
        $identity.session_id -ne (Get-Process -Id $PID).SessionId) { throw 'maintenance_gui_identity_mismatch' }
    $formal = Get-UpgradeDirectory (Split-Path $identity.path)
    $workingItem = Get-Item -LiteralPath $context.working_directory
    if (-not $workingItem.PSIsContainer) { throw 'maintenance_working_directory_missing' }
    $workingDirectory = $workingItem.FullName
    if ($contextDirectory -eq $formal -or $contextDirectory.StartsWith($formal + '\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'maintenance_context_must_be_independent'
    }
    $candidate = $formal
    if ($Action -eq 'update') {
        if (-not $CandidateDirectory) { throw 'maintenance_candidate_required' }
        $candidate = Get-UpgradeDirectory $CandidateDirectory
        if ($candidate -eq $formal -or $candidate.StartsWith($formal + '\',[StringComparison]::OrdinalIgnoreCase) -or
            $formal.StartsWith($candidate + '\',[StringComparison]::OrdinalIgnoreCase) -or
            $contextDirectory -eq $candidate -or $contextDirectory.StartsWith($candidate + '\',[StringComparison]::OrdinalIgnoreCase)) {
            throw 'maintenance_directories_overlap'
        }
    } elseif ($CandidateDirectory) { throw 'maintenance_restart_has_no_candidate' }
    if (-not @($context.runtime_arguments).Count -or @($context.runtime_arguments) -contains '--gui-runtime-stdio') { throw 'maintenance_runtime_arguments_invalid' }
    $runtimeIdentities = @(Get-UpgradeRuntimeFamily $identity)
    $runtimeMatch = @($runtimeIdentities | Where-Object { $_.pid -eq [int]$context.runtime_pid })
    if ($runtimeMatch.Count -ne 1 -or [DateTimeOffset]::Parse($runtimeMatch[0].started).ToUnixTimeMilliseconds() -ne [long]$context.runtime_started_ms) {
        throw 'maintenance_runtime_session_changed'
    }
    $originalManifest = @(Get-UpgradeManifest $formal)
    Assert-UpgradeManifest $formal $originalManifest
    $candidateManifest = if ($Action -eq 'update') { @(Get-UpgradeManifest $candidate) } else { $originalManifest }
    Assert-UpgradeManifest $candidate $candidateManifest
    New-Item -ItemType Directory -Path $operationDirectory | Out-Null
    Protect-UpgradePath $operationDirectory
    $workerDirectory = Join-Path $operationDirectory 'worker'
    New-Item -ItemType Directory -Path $workerDirectory | Out-Null
    Protect-UpgradePath $workerDirectory
    foreach ($name in @('runtime-upgrade-common.ps1','invoke-runtime-directory-upgrade.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $workerDirectory $name)
        Protect-UpgradePath (Join-Path $workerDirectory $name)
    }
    $taskName = 'RedClaw-Maintenance-' + $OperationId
    $resumeArguments = @('--gui-maintenance-resume',[IO.Path]::GetFullPath($ContextPath))
    # The GUI owns the retained settings and exact runtime arguments. They stay
    # in a private local context instead of entering terminal/peer messages.
    $plan = [ordered]@{
        schema='redclaw.runtime-upgrade.plan.v2';operation_id=$OperationId;task_name=$taskName;action=$Action;
        context_path=[IO.Path]::GetFullPath($ContextPath);target=$identity;runtime_identities=$runtimeIdentities;
        formal_directory=$formal;candidate_directory=$candidate;working_directory=$workingDirectory;
        original_manifest=$originalManifest;candidate_manifest=$candidateManifest;
        original_arguments=$resumeArguments;updated_arguments=$resumeArguments;role='host';control_name='';
        status_path=$statusPath;acknowledgment_path=(Join-Path $operationDirectory 'handoff-ack.txt');
        rollback_directory=($formal+'.rollback.'+$OperationId);pending_directory=($formal+'.candidate.'+$OperationId)
    }
    $plan | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $planPath -Encoding UTF8
    $createdPlan = $true
    Protect-UpgradePath $planPath
    $planHash = (Get-FileHash -LiteralPath $planPath -Algorithm SHA256).Hash
    if ($PlanOnly) {
        [pscustomobject]@{phase='validated';operation_id=$OperationId;plan_path=$planPath;plan_sha256=$planHash;target_pid=$identity.pid} | ConvertTo-Json
        return
    }
    [ordered]@{schema='redclaw.runtime-maintenance.active.v1';operation_id=$OperationId} |
        ConvertTo-Json | Set-Content -LiteralPath $activePath -Encoding UTF8
    Protect-UpgradePath $activePath
    $shell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $worker = Join-Path $workerDirectory 'invoke-runtime-directory-upgrade.ps1'
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$worker,'-PlanPath',$planPath,'-PlanSha256',$planHash)
    $taskAction = New-ScheduledTaskAction -Execute $shell -Argument (($arguments | ForEach-Object { ConvertTo-UpgradeArgument $_ }) -join ' ')
    $principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $taskName -Action $taskAction -Principal $principal -Settings $settings | Out-Null
    $registeredTask = $true
    Start-ScheduledTask -TaskName $taskName
    $workerPid = Wait-UpgradeHandoff $identity $statusPath $plan.acknowledgment_path $planHash $shell $arguments
    [pscustomobject]@{phase='independent_worker_owns_upgrade';operation_id=$OperationId;worker_pid=$workerPid;status_path=$statusPath} | ConvertTo-Json
    return
} catch {
    $failure = $_.Exception.Message
    # Before acknowledgment the worker cannot stop the GUI. Stop only this
    # fixed task if launch/handoff fails, leaving the current Host untouched.
    if ($createdPlan -and -not (Test-Path -LiteralPath (Join-Path $operationDirectory 'handoff-ack.txt'))) {
        $ownedTask = 'RedClaw-Maintenance-' + $OperationId
        if ($registeredTask) {
            Stop-ScheduledTask -TaskName $ownedTask -ErrorAction SilentlyContinue
            Unregister-ScheduledTask -TaskName $ownedTask -Confirm:$false -ErrorAction SilentlyContinue
        }
        Write-UpgradeReceipt $statusPath 'failed' ([int]$context.gui_pid) $failure
    }
    throw
} finally { $lock.Dispose() }
