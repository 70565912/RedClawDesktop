param([Parameter(Mandatory=$true)][string]$RunDirectory)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$root=(Resolve-Path -LiteralPath $RunDirectory).Path
$reportsRoot=(Resolve-Path (Join-Path $repo 'build\reports')).Path
$runLeaf=Split-Path -Leaf $root
if (-not $root.StartsWith(($reportsRoot+'\'),[StringComparison]::OrdinalIgnoreCase) -or
    $runLeaf -notmatch '^local-dual-gui-\d{8}_\d{6}_\d{3}$') {throw 'Not a local test run.'}
$run=Get-Content -LiteralPath (Join-Path $root 'result.json') -Raw | ConvertFrom-Json
if ($run.run_id -notmatch '^\d{8}_\d{6}_\d{3}$') {throw 'Invalid Run ID.'}
$expectedExe=$repo+'\release\Debug\redclaw_desktop.exe'
if ($run.runtime_path -ne $expectedExe -or (Get-FileHash -LiteralPath $expectedExe).Hash -ne $run.runtime_sha256) {throw 'Package identity changed.'}
$qa=$repo+'\scripts\service\invoke-cross-lan-debug-control.ps1'
$owned=@{}
foreach ($role in @('Controller','Host')) {
    $name='RedClawDesktop.LocalGui.'+$role+'.'+$run.run_id
    $s=& $qa -ControlName $name -Action agent_status -Json | ConvertFrom-Json
    if (-not $s.ok) {throw 'QA unavailable; no stop.'}
    $guiId=if($role -eq 'Host') {$run.owned_host_pid} else {$run.owned_controller_pid}
    if ($s.status.app_pid -ne $guiId) {throw 'GUI identity mismatch.'}
    $runtimeId=$s.status.runtime_pid
    foreach ($id in @($guiId,$runtimeId)) {
        $c=Get-CimInstance Win32_Process -Filter ('ProcessId='+$id)
        if ($c.ExecutablePath -ne $expectedExe -or $c.CommandLine -notmatch [regex]::Escape($run.run_id) -or
            ($id -eq $runtimeId -and $c.ParentProcessId -ne $guiId)) {throw 'Run/path/parent mismatch.'}
        $p=Get-Process -Id $id
        [void]$p.Handle
        if ($p.Path -ne $expectedExe -or [math]::Abs(($p.StartTime-$c.CreationDate).TotalSeconds) -gt 1) {throw 'Reused PID.'}
        $owned[$id]=$p
    }
    $s | ConvertTo-Json -Depth 32 | Out-File -LiteralPath (Join-Path $root ('stop-before-'+$role+'.json')) -Encoding utf8
    $e=& $qa -ControlName $name -Action export_evidence -EvidenceTimeoutMs 120000 -Json | ConvertFrom-Json
    $e | ConvertTo-Json -Depth 32 | Out-File -LiteralPath (Join-Path $root ('final-evidence-'+$role+'.json')) -Encoding utf8
    if (-not $e.ok) {throw 'Evidence export failed; preserve running endpoint.'}
}
$exits=@()
foreach ($role in @('Controller','Host')) {
    $before=Get-Content -LiteralPath (Join-Path $root ('stop-before-'+$role+'.json')) -Raw | ConvertFrom-Json
    $name='RedClawDesktop.LocalGui.'+$role+'.'+$run.run_id
    $reply=& $qa -ControlName $name -Action stop -Json | ConvertFrom-Json
    if (-not $reply.ok) {throw 'QA stop rejected.'}
    $runtime=$owned[$before.status.runtime_pid]
    # Let the GUI's asynchronous terminate/kill timer finish before closing it.
    # Never interpret an accepted stop request as confirmation of child exit.
    if (-not $runtime.WaitForExit(20000)) {throw 'Runtime still running after bounded stop; leave GUI alive for diagnosis.'}
    $gui=$owned[$before.status.app_pid]
    $exitReply=& $qa -ControlName $name -Action exit -Json | ConvertFrom-Json
    if (-not $exitReply.ok) {throw 'GUI exit barrier rejected.'}
    if (-not $gui.WaitForExit(15000)) {throw 'GUI did not exit; do not force or publish.'}
    $exits += [pscustomobject]@{role=$role;runtime_pid=$runtime.Id;runtime_exit=$runtime.ExitCode;gui_pid=$gui.Id;gui_exit=$gui.ExitCode;all_exited=$true;forced_by_script=$false}
}
$exits | ConvertTo-Json | Tee-Object -FilePath (Join-Path $root 'owned-process-exits.json')
