#Requires -Version 7.0
<#
.SYNOPSIS
Calls the opted-in current-user RedClaw workspace endpoint.
.DESCRIPTION
Uses the running Controller's existing P2P session. A wait timeout never cancels
or resubmits an operation. Keep the returned operation_id for later queries.
Requires --enable-workspace-control when launching the candidate GUI.
.EXAMPLE
.\invoke-workspace-control.ps1 -Operation terminal.open
.EXAMPLE
.\invoke-workspace-control.ps1 -Operation terminal.exec -Parameters @{command='Get-Location'} -WaitSeconds 10
.EXAMPLE
.\invoke-workspace-control.ps1 -Operation operation.result -Parameters @{operation_id='...';cursor='0'}
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.:-]{1,128}$')][string]$Operation,
    [hashtable]$Parameters = @{},
    [string]$ParametersJson = '',
    [ValidatePattern('^[A-Za-z0-9_.:-]{1,128}$')][string]$RequestId = ([guid]::NewGuid().ToString('N')),
    [ValidatePattern('^[A-Za-z0-9_.-]{1,128}$')][string]$PipeName = 'RedClawDesktop.WorkspaceControl.v1',
    [ValidateRange(0, 86400)][int]$WaitSeconds = 0,
    [ValidateRange(100, 30000)][int]$ConnectionTimeoutMs = 5000,
    [string]$InstanceId = ''
)
$ErrorActionPreference = 'Stop'
if ($ParametersJson) {
    if ($Parameters.Count) { throw 'Specify Parameters or ParametersJson, not both.' }
    $Parameters = ConvertFrom-Json -InputObject $ParametersJson -AsHashtable
}
function Invoke-WorkspaceFrame {
    param([hashtable]$Frame, [string]$Endpoint, [int]$TimeoutMs)
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $bytes = $utf8.GetBytes((ConvertTo-Json -InputObject $Frame -Depth 20 -Compress) + "`n")
    if ($bytes.Length -gt 65536) { throw 'workspace_request_too_large' }
    $options = [IO.Pipes.PipeOptions]::Asynchronous -bor [IO.Pipes.PipeOptions]::CurrentUserOnly
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', $Endpoint, [IO.Pipes.PipeDirection]::InOut, $options)
    try {
        $pipe.Connect($TimeoutMs)
        $pipe.Write($bytes, 0, $bytes.Length)
        $pipe.Flush()
        $buffer = [byte[]]::new(4096)
        $reply = [IO.MemoryStream]::new()
        try {
            while ($true) {
                $read = $pipe.ReadAsync($buffer, 0, $buffer.Length)
                if (-not $read.Wait($TimeoutMs)) { throw 'workspace_reply_timeout_result_unknown_do_not_resubmit' }
                $count = $read.Result
                if (-not $count) { throw 'workspace_disconnected_result_unknown' }
                $reply.Write($buffer, 0, $count)
                if ($reply.Length -gt 262144) { throw 'workspace_reply_too_large' }
                if ([Array]::IndexOf($buffer, [byte]10, 0, $count) -ge 0) {
                    return ConvertFrom-Json -InputObject $utf8.GetString($reply.ToArray())
                }
            }
        } finally { $reply.Dispose() }
    } finally { $pipe.Dispose() }
}
$capabilities = Invoke-WorkspaceFrame -Endpoint $PipeName -TimeoutMs $ConnectionTimeoutMs -Frame @{
    version=1; request_id=([guid]::NewGuid().ToString('N')); operation='capabilities'; parameters=@{}
}
if (-not $capabilities.ok) { return $capabilities }
if ($InstanceId -and $InstanceId -ne $capabilities.instance_id) { throw 'workspace_instance_changed' }
$epoch = [string]$capabilities.instance_id
$result = Invoke-WorkspaceFrame -Endpoint $PipeName -TimeoutMs $ConnectionTimeoutMs -Frame @{
    version=1; instance_id=$epoch; request_id=$RequestId; operation=$Operation; parameters=$Parameters
}
if ($WaitSeconds -le 0 -or -not $result.ok -or -not $result.operation_id) { return $result }
$operationId = [string]$result.operation_id
$timer = [Diagnostics.Stopwatch]::StartNew()
while ($timer.Elapsed.TotalSeconds -lt $WaitSeconds) {
    $result = Invoke-WorkspaceFrame -Endpoint $PipeName -TimeoutMs $ConnectionTimeoutMs -Frame @{
        version=1; instance_id=$epoch; request_id=([guid]::NewGuid().ToString('N'));
        operation='operation.status'; parameters=@{operation_id=$operationId}
    }
    if (-not $result.ok -or $result.state -in @('succeeded','failed','cancelled','unknown','rejected')) { return $result }
    Start-Sleep -Milliseconds 100
}
$result | Add-Member -NotePropertyName wait_timed_out -NotePropertyValue $true -Force
return $result
