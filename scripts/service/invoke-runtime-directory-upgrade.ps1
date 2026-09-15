param(
    [Parameter(Mandatory)][string]$PlanPath,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$PlanSha256
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
Assert-UpgradeOwner $PlanPath
if ((Get-FileHash -LiteralPath $PlanPath -Algorithm SHA256).Hash -ne $PlanSha256) { throw 'upgrade_plan_hash_mismatch' }
$plan = Get-Content -LiteralPath $PlanPath -Raw | ConvertFrom-Json
if ($plan.schema -ne 'redclaw.runtime-upgrade.plan.v1' -or $plan.operation_id -notmatch '^[0-9a-f]{32}$') { throw 'upgrade_plan_schema_invalid' }
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
    Assert-UpgradeManifest $candidate @($plan.candidate_manifest)
    Assert-UpgradeManifest $formal @($plan.original_manifest)
    Assert-UpgradeIdentity $plan.target
    Assert-UpgradeRuntimeFamily $plan.target @($plan.runtime_identities)
    if ((& git -C $plan.repo_root rev-parse HEAD).Trim() -ne $plan.expected_git_sha) { throw 'upgrade_repository_commit_changed' }
    Copy-Item -LiteralPath $candidate -Destination $plan.pending_directory -Recurse
    Assert-UpgradeManifest $plan.pending_directory @($plan.candidate_manifest)
    $probe = Start-Process -FilePath (Join-Path $plan.pending_directory 'redclaw_desktop.exe') -ArgumentList '--help' -PassThru -WindowStyle Hidden
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
    Assert-UpgradeIdentity $plan.target
    Assert-UpgradeManifest $plan.pending_directory @($plan.candidate_manifest)
    Write-UpgradeReceipt $plan.status_path 'stopping' $plan.target.pid
    Assert-UpgradeRuntimeFamily $plan.target @($plan.runtime_identities)
    Stop-UpgradeGui $plan.target $plan.control_name $plan.role
    $stopped = $true
    foreach ($entry in $plan.original_manifest) {
        $handle = [IO.File]::Open((Join-Path $formal $entry.path),'Open','Read','None')
        $handle.Dispose()
    }
    # All resolved directory moves remain explicit siblings of the formal path.
    Move-Item -LiteralPath $formal -Destination $plan.rollback_directory
    try { Move-Item -LiteralPath $plan.pending_directory -Destination $formal }
    catch { Move-Item -LiteralPath $plan.rollback_directory -Destination $formal; throw }
    $swapped = $true
    Assert-UpgradeManifest $formal @($plan.candidate_manifest)
    Write-UpgradeReceipt $plan.status_path 'starting' 0
    $newProcess = Start-UpgradedGui (Join-Path $formal 'redclaw_desktop.exe') @($plan.updated_arguments) $plan.repo_root
    if (-not (Test-UpgradedGui $newProcess)) { throw 'upgrade_gui_start_failed' }
    # Network establishment has its own diagnostics. A running GUI waiting for
    # DHT/ICE is not a reason to repeatedly roll back a valid binary bundle.
    Write-UpgradeReceipt $plan.status_path 'completed' $newProcess.Id 'gui_healthy_connection_checked_separately'
} catch {
    $failure = $_.Exception.Message
    if ($swapped) {
        if ($newProcess -and -not $newProcess.HasExited) {
            $newIdentity = Get-UpgradeIdentity $newProcess.Id
            Stop-UpgradeGui $newIdentity $plan.control_name $plan.role
        }
        $failedDirectory = $formal + '.failed.' + $plan.operation_id
        if ((Split-Path ([IO.Path]::GetFullPath($failedDirectory))) -ne $parent -or (Test-Path -LiteralPath $failedDirectory)) { throw 'upgrade_rollback_target_invalid' }
        Move-Item -LiteralPath $formal -Destination $failedDirectory
        Move-Item -LiteralPath $plan.rollback_directory -Destination $formal
        Assert-UpgradeManifest $formal @($plan.original_manifest)
    }
    if ($stopped) {
        $restored = Start-UpgradedGui (Join-Path $formal 'redclaw_desktop.exe') @($plan.original_arguments) $plan.repo_root
        $healthy = Test-UpgradedGui $restored
        Write-UpgradeReceipt $plan.status_path $(if($healthy){'rolled_back'}else{'rollback_start_failed'}) $restored.Id $failure
    } else { Write-UpgradeReceipt $plan.status_path 'failed' $plan.target.pid $failure }
    throw
} finally {
    Unregister-ScheduledTask -TaskName $plan.task_name -Confirm:$false -ErrorAction SilentlyContinue
}
