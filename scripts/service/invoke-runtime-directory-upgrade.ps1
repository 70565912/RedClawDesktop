param(
    [Parameter(Mandatory)][string]$PlanPath,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$PlanSha256
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
Assert-UpgradeOwner $PlanPath
if ((Get-FileHash -LiteralPath $PlanPath -Algorithm SHA256).Hash -ne $PlanSha256) { throw 'upgrade_plan_hash_mismatch' }
$plan = Get-Content -LiteralPath $PlanPath -Raw | ConvertFrom-Json
if ($plan.schema -notin @('redclaw.runtime-upgrade.plan.v1','redclaw.runtime-upgrade.plan.v2') -or $plan.operation_id -notmatch '^[0-9a-f]{32}$') { throw 'upgrade_plan_schema_invalid' }
$portable = $plan.schema -eq 'redclaw.runtime-upgrade.plan.v2'
if ($portable -and $plan.action -notin @('restart','update')) { throw 'upgrade_action_invalid' }
$isUpdate = -not $portable -or $plan.action -eq 'update'
$requireRuntime = $portable -or @($plan.runtime_identities).Count -gt 0
$workingDirectory = if ($portable) { (Get-Item -LiteralPath $plan.working_directory).FullName } else { $plan.repo_root }
$formal = Get-UpgradeDirectory $plan.formal_directory
$candidate = Get-UpgradeDirectory $plan.candidate_directory
$parent = Split-Path $formal
foreach ($moveTarget in @($plan.rollback_directory,$plan.pending_directory)) {
    $resolvedTarget = [IO.Path]::GetFullPath($moveTarget)
    if ((Split-Path $resolvedTarget) -ne $parent -or -not $resolvedTarget.EndsWith($plan.operation_id) -or (Test-Path -LiteralPath $resolvedTarget)) { throw 'upgrade_move_target_invalid' }
}
$swapped = $false
$stopped = $false
$newProcess = $null
try {
    Write-UpgradeReceipt $plan.status_path 'preparing' $plan.target.pid 'validating_candidate'
    if ($isUpdate) { Assert-UpgradeManifest $candidate @($plan.candidate_manifest) }
    Write-UpgradeReceipt $plan.status_path 'preparing' $plan.target.pid 'validating_original'
    Assert-UpgradeManifest $formal @($plan.original_manifest)
    Assert-UpgradeIdentity $plan.target
    Assert-UpgradeRuntimeFamily $plan.target @($plan.runtime_identities)
    if (-not $portable -and (& git -C $plan.repo_root rev-parse HEAD).Trim() -ne $plan.expected_git_sha) { throw 'upgrade_repository_commit_changed' }
    $probeDirectory = $formal
    if ($isUpdate) {
        Write-UpgradeReceipt $plan.status_path 'preparing' $plan.target.pid 'copying_candidate'
        Copy-Item -LiteralPath $candidate -Destination $plan.pending_directory -Recurse
        Write-UpgradeReceipt $plan.status_path 'preparing' $plan.target.pid 'validating_pending'
        Assert-UpgradeManifest $plan.pending_directory @($plan.candidate_manifest)
        $probeDirectory = $plan.pending_directory
    }
    Write-UpgradeReceipt $plan.status_path 'preparing' $plan.target.pid 'probing_candidate'
    $probe = Start-Process -FilePath (Join-Path $probeDirectory 'redclaw_desktop.exe') -ArgumentList '--help' -PassThru -WindowStyle Hidden
    try {
        if (-not $probe.WaitForExit(15000) -or $probe.ExitCode -ne 0) { throw 'upgrade_candidate_start_probe_failed' }
    } finally {
        # Own this exact Process object; a hung package probe must not keep the
        # candidate bundle locked after the upgrade preflight has failed.
        if (-not $probe.HasExited) { $probe.Kill(); [void]$probe.WaitForExit(5000) }
        $probe.Dispose()
    }
    Write-UpgradeReceipt $plan.status_path 'handoff_ready' $plan.target.pid
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(60)
    while (-not (Test-Path -LiteralPath $plan.acknowledgment_path)) {
        if ([DateTimeOffset]::UtcNow -gt $deadline) { throw 'upgrade_handoff_not_acknowledged' }
        Start-Sleep -Milliseconds 200
    }
    if ((Get-Content -LiteralPath $plan.acknowledgment_path -Raw).Trim() -ne $PlanSha256) { throw 'upgrade_handoff_hash_mismatch' }
    if ($isUpdate) { Assert-UpgradeManifest $plan.pending_directory @($plan.candidate_manifest) }
    else { Assert-UpgradeManifest $formal @($plan.original_manifest) }
    Write-UpgradeReceipt $plan.status_path 'stopping' $plan.target.pid
    if (Get-Process -Id $plan.target.pid -ErrorAction SilentlyContinue) {
        Assert-UpgradeIdentity $plan.target
        Assert-UpgradeRuntimeFamily $plan.target @($plan.runtime_identities)
        Stop-UpgradeGui $plan.target $plan.control_name $plan.role
    } else {
        # Ownership already transferred. If the initiating Host has exited,
        # wait for its runtime to release the bundle; never act on a reused PID.
        $deadline = [DateTimeOffset]::UtcNow.AddSeconds(20)
        do {
            $remaining = @(Get-CimInstance Win32_Process -Filter "Name='redclaw_desktop.exe'" | Where-Object {$_.ExecutablePath -eq $plan.target.path})
            if (-not $remaining.Count) { break }
            Start-Sleep -Milliseconds 200
        } while ([DateTimeOffset]::UtcNow -lt $deadline)
        if ($remaining.Count) { throw 'upgrade_runtime_files_still_owned' }
    }
    $stopped = $true
    Wait-UpgradeBundleReleased $formal @($plan.original_manifest)
    if ($isUpdate) {
        # All resolved directory moves remain explicit siblings of the formal path.
        Move-Item -LiteralPath $formal -Destination $plan.rollback_directory
        try { Move-Item -LiteralPath $plan.pending_directory -Destination $formal }
        catch { Move-Item -LiteralPath $plan.rollback_directory -Destination $formal; throw }
        $swapped = $true
        Assert-UpgradeManifest $formal @($plan.candidate_manifest)
    }
    Write-UpgradeReceipt $plan.status_path 'starting' 0
    $newProcess = Start-UpgradedGui (Join-Path $formal 'redclaw_desktop.exe') @($plan.updated_arguments) $workingDirectory
    if (-not (Test-UpgradedGui $newProcess -RequireHostRuntime:$requireRuntime)) { throw 'upgrade_gui_or_runtime_start_failed' }
    # Network establishment has its own diagnostics. A running GUI waiting for
    # DHT/ICE is not a reason to repeatedly roll back a valid binary bundle.
    Write-UpgradeReceipt $plan.status_path 'completed' $newProcess.Id 'gui_healthy_connection_checked_separately'
} catch {
    $failure = $_.Exception.Message
    try {
    if ($newProcess -and -not $newProcess.HasExited) {
        $newIdentity = Get-UpgradeIdentity $newProcess.Id
        Stop-UpgradeGui $newIdentity $plan.control_name $plan.role
    }
    if ($swapped) {
        $failedDirectory = $formal + '.failed.' + $plan.operation_id
        if ((Split-Path ([IO.Path]::GetFullPath($failedDirectory))) -ne $parent -or (Test-Path -LiteralPath $failedDirectory)) { throw 'upgrade_rollback_target_invalid' }
        Move-Item -LiteralPath $formal -Destination $failedDirectory
        Move-Item -LiteralPath $plan.rollback_directory -Destination $formal
        Assert-UpgradeManifest $formal @($plan.original_manifest)
    }
    if ($stopped -and $isUpdate) {
        $restored = Start-UpgradedGui (Join-Path $formal 'redclaw_desktop.exe') @($plan.original_arguments) $workingDirectory
        $healthy = Test-UpgradedGui $restored -RequireHostRuntime:$requireRuntime
        Write-UpgradeReceipt $plan.status_path $(if($healthy){'rolled_back'}else{'rollback_start_failed'}) $restored.Id $failure
    } else { Write-UpgradeReceipt $plan.status_path $(if($stopped){'restart_start_failed'}else{'failed'}) $plan.target.pid $failure }
    } catch {
        # A refused graceful close or rollback must still leave a final receipt.
        # Keep the running process/directory intact when recovery cannot own it.
        $remainingPid = if ($newProcess -and -not $newProcess.HasExited) { $newProcess.Id } else { $plan.target.pid }
        Write-UpgradeReceipt $plan.status_path 'failed' $remainingPid ($failure + '; recovery_failed: ' + $_.Exception.Message)
    }
    throw
} finally {
    Unregister-ScheduledTask -TaskName $plan.task_name -Confirm:$false -ErrorAction SilentlyContinue
}
