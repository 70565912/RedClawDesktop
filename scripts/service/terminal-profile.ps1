[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidGlobalVars', '', Justification='The interactive shell and its prompt/read-line callbacks must share session-scoped integration state.')]
param([ValidatePattern('^[a-zA-Z0-9-]*$')][string]$IntegrationNonce = '', [long]$IntegrationHandle = 0)

# Fixed records are emitted by the shell, never inferred from rendered prompts.
$global:__rc_nonce = $IntegrationNonce
$global:__rc_id = ''
$global:__rc_ok = $false
$global:__rc_complete = $false
# A launcher can inherit Windows' "ignore Ctrl+C" process flag into this
# console. Clear it in this Shell only; changing console input mode alone
# does not restore PowerShell's interrupt handler.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RedClawConsoleControl {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
}
'@
if (-not [RedClawConsoleControl]::SetConsoleCtrlHandler([IntPtr]::Zero, $false)) {
    throw 'terminal_interrupt_initialization_failed'
}
$global:__rc_stream = if ($IntegrationHandle) {
    $handle = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new([IntPtr]::new($IntegrationHandle), $true)
    [IO.FileStream]::new($handle, [IO.FileAccess]::Write, 4096, $false)
} else { $null }
function global:Write-RedClawRecord([string]$Record) {
    if ($null -eq $global:__rc_stream) { return }
    $bytes = [Text.Encoding]::UTF8.GetBytes($Record)
    $global:__rc_stream.Write($bytes, 0, $bytes.Length)
    $global:__rc_stream.Flush()
}
if (Get-Command Set-PSReadLineOption -ErrorAction SilentlyContinue) {
    Set-PSReadLineOption -HistorySaveStyle SaveNothing
}
function global:Get-RedClawBoundary {
    param([ValidateSet('start','done','ready')][string]$Kind)
    if (-not $global:__rc_nonce) { return }
    $native = Get-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue
    $code = if ($null -ne $native) { [string]$native.Value } else { '' }
    $success = if ($global:__rc_ok) { '1' } else { '0' }
    return ([char]27).ToString() + ']777;redclaw-v1;' + $global:__rc_nonce + ';' + $Kind + ';' + $global:__rc_id + ';' + $success + ';' + $code + [char]7
}
function global:Write-RedClawBoundary {
    param([ValidateSet('start','done','ready')][string]$Kind)
    # PSReadLine can leave raw Ctrl+C input enabled while an interactive
    # pipeline starts. API execution uses the console's normal interrupt path.
    if ($Kind -eq 'start') { [Console]::TreatControlCAsInput = $false }
    Write-RedClawRecord (Get-RedClawBoundary $Kind)
}
# PSReadLine invokes prompt again while repainting an unfinished input line.
# Readiness belongs to the main read loop, never to the prompt renderer.
$global:__rc_readline = (Get-Command PSConsoleHostReadLine -ErrorAction Stop).ScriptBlock
function global:PSConsoleHostReadLine {
    if ($NestedPromptLevel -eq 0) {
        if ($global:__rc_complete) { Write-RedClawBoundary done }
        Write-RedClawBoundary ready
        $global:__rc_id = ''
        $global:__rc_complete = $false
    }
    & $global:__rc_readline
}
function global:Write-RedClawExecOutput {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '', Justification='This fixed pipeline sink writes directly to the owned ConPTY console after journaling; Write-Output would feed the pipeline again.')]
    param([Parameter(ValueFromPipeline=$true)][AllowEmptyString()][string]$Text)
    process {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text + [Environment]::NewLine)
        for ($offset = 0; $offset -lt $bytes.Length; $offset += 4096) {
            $count = [Math]::Min(4096, $bytes.Length - $offset)
            $data = [Convert]::ToBase64String($bytes, $offset, $count)
            Write-RedClawRecord (([char]27).ToString() + ']777;redclaw-v1;' + $global:__rc_nonce + ';output;' + $global:__rc_id + ';0;' + $data + [char]7)
        }
        [Console]::WriteLine($Text)
        [Console]::Out.Flush()
    }
}

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
