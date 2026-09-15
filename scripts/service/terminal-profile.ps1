# Fixed local maintenance commands. Ordinary shell commands remain in the
# terminal job; only an acknowledged scheduled worker survives Host shutdown.
function global:Restart-RedClawHost {
    param([string]$CandidateDirectory = '', [ValidatePattern('^[0-9a-f]{32}$')][string]$OperationId = ([guid]::NewGuid().ToString('N')))
    if (-not $env:REDCLAW_RUNTIME_MAINTENANCE_CONTEXT) { throw 'maintenance_context_unavailable' }
    $arguments = @{
        ContextPath=$env:REDCLAW_RUNTIME_MAINTENANCE_CONTEXT;
        Action=$(if($CandidateDirectory){'update'}else{'restart'});
        OperationId=$OperationId
    }
    if ($CandidateDirectory) { $arguments.CandidateDirectory = $CandidateDirectory }
    & (Join-Path $PSScriptRoot 'start-runtime-maintenance.ps1') @arguments
}
function global:Get-RedClawMaintenance {
    if (-not $env:REDCLAW_RUNTIME_MAINTENANCE_CONTEXT) { throw 'maintenance_context_unavailable' }
    . (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
    Assert-UpgradeOwner $env:REDCLAW_RUNTIME_MAINTENANCE_CONTEXT
    $directory = Split-Path $env:REDCLAW_RUNTIME_MAINTENANCE_CONTEXT
    $activePath = Join-Path $directory 'active-operation.json'
    if (-not (Test-Path -LiteralPath $activePath)) {
        $context = Get-Content -LiteralPath $env:REDCLAW_RUNTIME_MAINTENANCE_CONTEXT -Raw | ConvertFrom-Json
        if ($context.PSObject.Properties.Name -contains 'previous_context' -and $context.previous_context) {
            Assert-UpgradeOwner $context.previous_context
            $directory = Split-Path $context.previous_context
            $activePath = Join-Path $directory 'active-operation.json'
        }
    }
    if (-not (Test-Path -LiteralPath $activePath)) { return [pscustomobject]@{phase='idle'} }
    Assert-UpgradeOwner $activePath
    $active = Get-Content -LiteralPath $activePath -Raw | ConvertFrom-Json
    if ($active.schema -ne 'redclaw.runtime-maintenance.active.v1' -or $active.operation_id -notmatch '^[0-9a-f]{32}$') {
        throw 'maintenance_active_receipt_invalid'
    }
    $status = Join-Path (Join-Path $directory $active.operation_id) 'status.json'
    if (Test-Path -LiteralPath $status) { Get-UpgradeReceipt $status }
    else { [pscustomobject]@{phase='preparing';operation_id=$active.operation_id} }
}
Write-Output 'Host maintenance: Restart-RedClawHost [-CandidateDirectory <complete runtime folder>]; Get-RedClawMaintenance'
