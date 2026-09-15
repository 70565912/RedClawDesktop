param(
    [string]$EvidenceDirectory = '',
    [switch]$PortableMaintenance,
    [ValidateSet('identity_mismatch','corrupt_bundle','file_occupied','startup_rollback','parent_exit','target_exit_after_handoff','delayed_runtime','delayed_window','recovery_refused','delayed_file_release')]
    [string[]]$Scenarios = @('identity_mismatch','corrupt_bundle','file_occupied','startup_rollback','parent_exit')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $EvidenceDirectory) { $EvidenceDirectory = Join-Path $repo ('build/reports/runtime-upgrade-test-' + (Get-Date -Format yyyyMMdd-HHmmss)) }
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$root = Get-UpgradeDirectory $EvidenceDirectory
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$shell = (Get-Process -Id $PID).Path
$launcher = Join-Path $PSScriptRoot 'start-agent-runtime-upgrade.ps1'
$worker = Join-Path $PSScriptRoot 'invoke-runtime-directory-upgrade.ps1'
$gitSha = (& git -C $repo rev-parse HEAD).Trim()
$results = [Collections.Generic.List[object]]::new()
$ownedProcesses = [Collections.Generic.List[int]]::new()

function New-FixtureBundle {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseShouldProcessForStateChangingFunctions','',Justification='Isolated fixture owned and cleaned up by this test.') ]
    [CmdletBinding()]
    param([string]$Directory, [bool]$FailStart, [bool]$DelayRuntime = $false, [bool]$DelayWindow = $false, [bool]$RefuseRecovery = $false)
    New-Item -ItemType Directory -Path (Join-Path $Directory 'platforms') -Force | Out-Null
    $source = Join-Path $Directory 'fixture.cs'
    $template = @'
using System;
using System.Diagnostics;
using System.Threading;
using System.Windows.Forms;
class Fixture {
    [STAThread] static int Main(string[] args) {
        if (Array.IndexOf(args,"--help")>=0) return 0;
        if (Array.IndexOf(args,"--fixture-runtime")>=0) { Thread.Sleep(Timeout.Infinite); return 0; }
        if (FAIL_START) return 23;
        if (DELAY_WINDOW) Thread.Sleep(20000);
        Process runtime = null;
        var form = new Form {Text="RedClaw Upgrade Test Fixture", Width=360, Height=120};
        var startup = new System.Windows.Forms.Timer { Interval=DELAY_RUNTIME ? 8000 : 1 };
        startup.Tick += delegate { startup.Stop(); if (PORTABLE && !REFUSE_RECOVERY) runtime = Process.Start(new ProcessStartInfo(Application.ExecutablePath, "--fixture-runtime") { UseShellExecute=false, CreateNoWindow=true }); };
        if (REFUSE_RECOVERY) form.FormClosing += delegate(object sender, FormClosingEventArgs close) { close.Cancel = true; };
        startup.Start();
        form.Controls.Add(new Label {Text="Isolated upgrade test. No remote session.",Dock=DockStyle.Fill});
        try { Application.Run(form); return 0; }
        finally { startup.Dispose(); if (runtime != null) { if (!runtime.HasExited) { runtime.Kill(); runtime.WaitForExit(5000); } runtime.Dispose(); } }
    }
}
'@
    $template.Replace('FAIL_START', $FailStart.ToString().ToLowerInvariant()).Replace('PORTABLE', $PortableMaintenance.IsPresent.ToString().ToLowerInvariant()).Replace('DELAY_RUNTIME', $DelayRuntime.ToString().ToLowerInvariant()).Replace('DELAY_WINDOW', $DelayWindow.ToString().ToLowerInvariant()).Replace('REFUSE_RECOVERY', $RefuseRecovery.ToString().ToLowerInvariant()) | Set-Content -LiteralPath $source -Encoding UTF8
    & $compiler /nologo /target:winexe /r:System.Windows.Forms.dll /r:System.Drawing.dll "/out:$Directory/redclaw_desktop.exe" $source | Out-Null
    if ($LASTEXITCODE) { throw 'fixture_compile_failed' }
    Remove-Item -LiteralPath $source
    foreach ($name in @('redclaw_protocol_codec.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','platforms/qwindows.dll')) {
        ('fixture-' + $FailStart) | Set-Content -LiteralPath (Join-Path $Directory $name) -Encoding ASCII
    }
}

function Start-FixtureGui {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseShouldProcessForStateChangingFunctions','',Justification='Isolated fixture owned and cleaned up by this test.') ]
    [CmdletBinding()]
    param([string]$Directory)
    $gui = Start-Process -FilePath (Join-Path $Directory 'redclaw_desktop.exe') -ArgumentList '--gui-role host' -PassThru -WindowStyle Normal
    $ownedProcesses.Add($gui.Id)
    if (-not (Test-UpgradedGui $gui)) { throw 'fixture_gui_start_failed' }
    return $gui
}

function Wait-FixtureReceipt {
    param([string]$Path, [int]$TimeoutSeconds = 90)
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (Test-Path -LiteralPath $Path) {
            try { $receipt=Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json } catch { $receipt=$null }
            if ($receipt -and $receipt.phase -in @('completed','failed','rolled_back','rollback_start_failed','restart_start_failed')) { return $receipt }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw 'fixture_receipt_timeout'
}

function New-FixtureMaintenanceContext {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseShouldProcessForStateChangingFunctions','',Justification='Isolated fixture owned and cleaned up by this test.') ]
    [CmdletBinding()]
    param([string]$Directory, [Diagnostics.Process]$Gui)
    New-Item -ItemType Directory -Path $Directory | Out-Null
    Protect-UpgradePath $Directory
    $identity = Get-UpgradeIdentity $Gui.Id
    $family = @(Get-UpgradeRuntimeFamily $identity)
    if ($family.Count -ne 1) { throw 'fixture_runtime_missing' }
    $contextPath = Join-Path $Directory 'session.json'
    [ordered]@{
        schema='redclaw.runtime-maintenance.context.v1';role='host';
        gui_pid=$Gui.Id;gui_started_ms=[DateTimeOffset]::Parse($identity.started).ToUnixTimeMilliseconds();
        runtime_pid=$family[0].pid;runtime_started_ms=[DateTimeOffset]::Parse($family[0].started).ToUnixTimeMilliseconds();
        executable=$identity.path;working_directory=$root;gui_arguments=@('--gui-role','host');runtime_arguments=@('--fixture-runtime')
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $contextPath -Encoding UTF8
    Protect-UpgradePath $contextPath
    return $contextPath
}

function New-FixtureAgentJob {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseShouldProcessForStateChangingFunctions','',Justification='Isolated fixture owned and cleaned up by this test.') ]
    [CmdletBinding()]
    param([Diagnostics.Process]$Process)
    if (-not ('RedClawFixtureAgentJob' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class RedClawFixtureAgentJob {
    [StructLayout(LayoutKind.Sequential)] struct Basic {
        public long processTime, jobTime; public uint flags;
        public UIntPtr minWork, maxWork; public uint processes;
        public UIntPtr affinity; public uint priority, scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] struct Limits {
        public Basic basic;
        public ulong readOps, writeOps, otherOps, readBytes, writeBytes, otherBytes;
        public UIntPtr processMemory, jobMemory, peakProcess, peakJob;
    }
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job, int type, ref Limits limits, uint size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    public static IntPtr Attach(IntPtr process) {
        var job = CreateJobObject(IntPtr.Zero, null);
        var limits = new Limits(); limits.basic.flags = 0x2000; // KILL_ON_JOB_CLOSE, as in the Agent.
        if (job == IntPtr.Zero || !SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf(limits))
            || !AssignProcessToJobObject(job, process)) {
            var error = Marshal.GetLastWin32Error(); if (job != IntPtr.Zero) CloseHandle(job);
            throw new Win32Exception(error);
        }
        return job;
    }
}
'@
    }
    return [RedClawFixtureAgentJob]::Attach($Process.Handle)
}

try {
    foreach ($scenario in $Scenarios) {
        $caseRoot = Join-Path $root $scenario
        $formal = Join-Path $caseRoot 'formal'
        $candidate = Join-Path $caseRoot 'candidate'
        New-FixtureBundle $formal $false
        New-FixtureBundle -Directory $candidate -FailStart ($scenario -eq 'startup_rollback') -DelayRuntime ($scenario -eq 'delayed_runtime') -DelayWindow ($scenario -eq 'delayed_window') -RefuseRecovery ($scenario -eq 'recovery_refused')
        $gui = Start-FixtureGui $formal
        $contextPath = ''
        if ($PortableMaintenance) {
            $contextPath = New-FixtureMaintenanceContext (Join-Path $caseRoot 'context') $gui
            $planResult = & (Join-Path $PSScriptRoot 'start-runtime-maintenance.ps1') -ContextPath $contextPath -Action update -CandidateDirectory $candidate -PlanOnly | ConvertFrom-Json
        } else {
            $planResult = & $launcher -TargetPid $gui.Id -CandidateDirectory $candidate -ExpectedGitSha $gitSha -EvidenceDirectory (Join-Path $caseRoot 'evidence') -BuildGatePassed -FocusedTestGatePassed -PlanOnly | ConvertFrom-Json
        }
        $plan = Get-Content -LiteralPath $planResult.plan_path -Raw | ConvertFrom-Json
        if ($scenario -eq 'identity_mismatch') { $plan.target.started = [DateTimeOffset]::UtcNow.AddDays(-1).ToString('o') }
        $plan | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $planResult.plan_path -Encoding UTF8
        Protect-UpgradePath $planResult.plan_path
        $hash = (Get-FileHash -LiteralPath $planResult.plan_path -Algorithm SHA256).Hash
        if ($scenario -eq 'corrupt_bundle') { 'corrupt' | Set-Content -LiteralPath (Join-Path $candidate 'Qt6Core.dll') }
        $lock = $null
        if ($scenario -in @('file_occupied','delayed_file_release')) { $lock=[IO.File]::Open((Join-Path $formal 'Qt6Core.dll'),'Open','Read','Read') }
        try {
            if ($scenario -eq 'parent_exit') {
                # The launcher ends after handoff; only the independently
                # scheduled worker may then stop, replace and relaunch the GUI.
                $launchArguments=@('-NoProfile','-File',$launcher,'-TargetPid',[string]$gui.Id,'-CandidateDirectory',$candidate,'-ExpectedGitSha',$gitSha,'-EvidenceDirectory',(Join-Path $caseRoot 'parent-evidence'),'-BuildGatePassed','-FocusedTestGatePassed')
                $restartOperation = [guid]::NewGuid().ToString('N')
                if ($PortableMaintenance) {
                    $launchArguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'start-runtime-maintenance.ps1'),'-ContextPath',$contextPath,'-Action','restart','-OperationId',$restartOperation)
                }
                $parentOutput = Join-Path $caseRoot 'parent-output.json'
                $parentProcess = Start-Process -FilePath $shell -ArgumentList (($launchArguments | ForEach-Object {ConvertTo-UpgradeArgument $_}) -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $parentOutput -RedirectStandardError (Join-Path $caseRoot 'parent-error.log')
                $agentJob = New-FixtureAgentJob $parentProcess
                try {
                    if (-not $parentProcess.WaitForExit(60000) -or $parentProcess.ExitCode -ne 0) { throw 'fixture_parent_failed' }
                } finally { [void][RedClawFixtureAgentJob]::CloseHandle($agentJob) }
                $handoff = Get-Content -LiteralPath $parentOutput -Raw | ConvertFrom-Json
                $receipt = Wait-FixtureReceipt $handoff.status_path
                if ($receipt.target_pid -gt 0) { $ownedProcesses.Add([int]$receipt.target_pid) }
                if ($PortableMaintenance) {
                    $again = & (Join-Path $PSScriptRoot 'start-runtime-maintenance.ps1') -ContextPath $contextPath -Action restart -OperationId $restartOperation | ConvertFrom-Json
                    if ($again.phase -ne 'completed' -or $again.replayed -ne $false -or $again.status_path -ne $handoff.status_path) { throw 'fixture_restart_replayed' }
                    $previousContext = $contextPath
                    foreach ($generation in 1..2) {
                        $resumedContext = New-FixtureMaintenanceContext (Join-Path $caseRoot "resumed-context-$generation") (Get-Process -Id $receipt.target_pid)
                        $resumed = Get-Content -LiteralPath $resumedContext -Raw | ConvertFrom-Json
                        $resumed | Add-Member -NotePropertyName previous_context -NotePropertyValue $previousContext
                        $resumed | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $resumedContext -Encoding UTF8
                        Protect-UpgradePath $resumedContext
                        # PlanOnly makes a broken idempotency check reproducible
                        # without accidentally restarting the fixture a second time.
                        $sameOperation = & (Join-Path $PSScriptRoot 'start-runtime-maintenance.ps1') -ContextPath $resumedContext -Action restart -OperationId $restartOperation -PlanOnly | ConvertFrom-Json
                        if ($sameOperation.phase -ne 'completed' -or $sameOperation.status_path -ne $handoff.status_path) { throw 'fixture_resumed_context_would_replay_restart' }
                        $previousContext = $resumedContext
                    }
                    Assert-UpgradeManifest $formal @($plan.original_manifest)
                }
            } else {
                if ($scenario -ne 'target_exit_after_handoff') { $hash | Set-Content -LiteralPath $plan.acknowledgment_path -Encoding ASCII }
                $launchArguments=@('-NoProfile','-File',$worker,'-PlanPath',$planResult.plan_path,'-PlanSha256',$hash)
                $workerProcess = Start-Process -FilePath $shell -ArgumentList (($launchArguments | ForEach-Object {ConvertTo-UpgradeArgument $_}) -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $caseRoot 'worker-output.log') -RedirectStandardError (Join-Path $caseRoot 'worker-error.log')
                if ($scenario -eq 'delayed_file_release') {
                    if (-not $gui.WaitForExit(30000)) { throw 'fixture_gui_did_not_stop' }
                    Start-Sleep -Milliseconds 2000
                    $lock.Dispose(); $lock = $null
                }
                if ($scenario -eq 'target_exit_after_handoff') {
                    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(30)
                    do {
                        $ready = $null
                        if (Test-Path -LiteralPath $plan.status_path) { try { $ready = Get-Content -LiteralPath $plan.status_path -Raw | ConvertFrom-Json } catch { $ready = $null } }
                        if ($ready -and $ready.phase -eq 'handoff_ready') { break }
                        Start-Sleep -Milliseconds 100
                    } while ([DateTimeOffset]::UtcNow -lt $deadline)
                    if (-not $ready -or $ready.phase -ne 'handoff_ready') { throw 'fixture_worker_not_ready' }
                    Stop-UpgradeGui $plan.target '' 'host'
                    $hash | Set-Content -LiteralPath $plan.acknowledgment_path -Encoding ASCII
                }
                $receipt = Wait-FixtureReceipt -Path $plan.status_path -TimeoutSeconds $(if($scenario -eq 'recovery_refused'){120}else{90})
                if (-not $workerProcess.WaitForExit(10000)) { throw 'fixture_worker_did_not_exit' }
            }
            $expected = switch ($scenario) { 'identity_mismatch' {'failed'} 'corrupt_bundle' {'failed'} 'file_occupied' {'rolled_back'} 'startup_rollback' {'rolled_back'} 'parent_exit' {'completed'} 'target_exit_after_handoff' {'completed'} 'delayed_runtime' {'completed'} 'delayed_window' {'completed'} 'recovery_refused' {'failed'} 'delayed_file_release' {'completed'} }
            if ($receipt.phase -ne $expected) { throw ("fixture_unexpected_phase: $scenario $($receipt.phase) $($receipt.detail)") }
            if ($receipt.target_pid -gt 0) { $ownedProcesses.Add([int]$receipt.target_pid) }
            $live = Get-Process -Id $receipt.target_pid -ErrorAction Stop
            Start-Sleep -Milliseconds 1500
            $live.Refresh()
            if ($live.HasExited) { throw 'fixture_gui_died_after_worker_exit' }
            if ($scenario -eq 'recovery_refused') {
                if ($receipt.detail -notmatch 'recovery_failed:') { throw 'fixture_recovery_failure_not_recorded' }
                if (@(Get-UpgradeRuntimeFamily (Get-UpgradeIdentity $live.Id)).Count -ne 0) { throw 'fixture_unexpected_runtime' }
            } elseif ($PortableMaintenance -and @(Get-UpgradeRuntimeFamily (Get-UpgradeIdentity $live.Id)).Count -ne 1) { throw 'fixture_runtime_died_after_worker_exit' }
            $results.Add([pscustomobject]@{scenario=$scenario;portable=$PortableMaintenance.IsPresent;pass=$true;phase=$receipt.phase;gui_survived_worker_exit=$true})
            [void]$live.CloseMainWindow()
            [void]$live.WaitForExit(5000)
            # This isolated fixture deliberately refuses WM_CLOSE and has no
            # runtime children. Dispose only its owned Process after verification.
            if ($scenario -eq 'recovery_refused' -and -not $live.HasExited) { $live.Kill(); [void]$live.WaitForExit(5000) }
        } finally { if ($lock) {$lock.Dispose()} }
    }
} finally {
    foreach ($processId in $ownedProcesses | Select-Object -Unique) {
        $process=Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($process -and $process.Path -and $process.Path.StartsWith($root + '\',[StringComparison]::OrdinalIgnoreCase)) {
            [void]$process.CloseMainWindow()
            [void]$process.WaitForExit(3000)
        }
    }
    $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $root 'results.json')
}
$results | ConvertTo-Json -Depth 4
