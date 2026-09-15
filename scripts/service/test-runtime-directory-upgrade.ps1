param(
    [string]$EvidenceDirectory = '',
    [ValidateSet('identity_mismatch','corrupt_bundle','file_occupied','startup_rollback','parent_exit')]
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
    param([string]$Directory, [bool]$FailStart)
    New-Item -ItemType Directory -Path (Join-Path $Directory 'platforms') -Force | Out-Null
    $source = Join-Path $Directory 'fixture.cs'
    $template = @'
using System;
using System.Windows.Forms;
class Fixture {
    [STAThread] static int Main(string[] args) {
        if (Array.IndexOf(args,"--help")>=0) return 0;
        if (FAIL_START) return 23;
        var form = new Form {Text="RedClaw Upgrade Test Fixture", Width=360, Height=120};
        form.Controls.Add(new Label {Text="Isolated upgrade test. No remote session.",Dock=DockStyle.Fill});
        Application.Run(form); return 0;
    }
}
'@
    $template.Replace('FAIL_START', $FailStart.ToString().ToLowerInvariant()) | Set-Content -LiteralPath $source -Encoding UTF8
    & $compiler /nologo /target:winexe /r:System.Windows.Forms.dll /r:System.Drawing.dll "/out:$Directory/redclaw_desktop.exe" $source | Out-Null
    if ($LASTEXITCODE) { throw 'fixture_compile_failed' }
    Remove-Item -LiteralPath $source
    foreach ($name in @('redclaw_protocol_codec.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','platforms/qwindows.dll')) {
        ('fixture-' + $FailStart) | Set-Content -LiteralPath (Join-Path $Directory $name) -Encoding ASCII
    }
}

function Start-FixtureGui {
    param([string]$Directory)
    $gui = Start-Process -FilePath (Join-Path $Directory 'redclaw_desktop.exe') -ArgumentList '--gui-role host' -PassThru -WindowStyle Normal
    $ownedProcesses.Add($gui.Id)
    if (-not (Test-UpgradedGui $gui)) { throw 'fixture_gui_start_failed' }
    return $gui
}

function Wait-FixtureReceipt {
    param([string]$Path)
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(90)
    do {
        if (Test-Path -LiteralPath $Path) {
            try { $receipt=Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json } catch { $receipt=$null }
            if ($receipt -and $receipt.phase -in @('completed','failed','rolled_back','rollback_start_failed')) { return $receipt }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw 'fixture_receipt_timeout'
}

function New-FixtureAgentJob {
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
        New-FixtureBundle $candidate ($scenario -eq 'startup_rollback')
        $gui = Start-FixtureGui $formal
        $planResult = & $launcher -TargetPid $gui.Id -CandidateDirectory $candidate -ExpectedGitSha $gitSha -EvidenceDirectory (Join-Path $caseRoot 'evidence') -BuildGatePassed -FocusedTestGatePassed -PlanOnly | ConvertFrom-Json
        $plan = Get-Content -LiteralPath $planResult.plan_path -Raw | ConvertFrom-Json
        if ($scenario -eq 'identity_mismatch') { $plan.target.started = [DateTimeOffset]::UtcNow.AddDays(-1).ToString('o') }
        $plan | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $planResult.plan_path -Encoding UTF8
        Protect-UpgradePath $planResult.plan_path
        $hash = (Get-FileHash -LiteralPath $planResult.plan_path -Algorithm SHA256).Hash
        if ($scenario -eq 'corrupt_bundle') { 'corrupt' | Set-Content -LiteralPath (Join-Path $candidate 'Qt6Core.dll') }
        $lock = $null
        if ($scenario -eq 'file_occupied') { $lock=[IO.File]::Open((Join-Path $formal 'Qt6Core.dll'),'Open','Read','Read') }
        try {
            if ($scenario -eq 'parent_exit') {
                # The launcher ends after handoff; only the independently
                # scheduled worker may then stop, replace and relaunch the GUI.
                $args=@('-NoProfile','-File',$launcher,'-TargetPid',[string]$gui.Id,'-CandidateDirectory',$candidate,'-ExpectedGitSha',$gitSha,'-EvidenceDirectory',(Join-Path $caseRoot 'parent-evidence'),'-BuildGatePassed','-FocusedTestGatePassed')
                $parentOutput = Join-Path $caseRoot 'parent-output.json'
                $parentProcess = Start-Process -FilePath $shell -ArgumentList (($args | ForEach-Object {ConvertTo-UpgradeArgument $_}) -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $parentOutput -RedirectStandardError (Join-Path $caseRoot 'parent-error.log')
                $agentJob = New-FixtureAgentJob $parentProcess
                try {
                    if (-not $parentProcess.WaitForExit(60000) -or $parentProcess.ExitCode -ne 0) { throw 'fixture_parent_failed' }
                } finally { [void][RedClawFixtureAgentJob]::CloseHandle($agentJob) }
                $handoff = Get-Content -LiteralPath $parentOutput -Raw | ConvertFrom-Json
                $receipt = Wait-FixtureReceipt $handoff.status_path
            } else {
                $hash | Set-Content -LiteralPath $plan.acknowledgment_path -Encoding ASCII
                $args=@('-NoProfile','-File',$worker,'-PlanPath',$planResult.plan_path,'-PlanSha256',$hash)
                $workerProcess = Start-Process -FilePath $shell -ArgumentList (($args | ForEach-Object {ConvertTo-UpgradeArgument $_}) -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $caseRoot 'worker-output.log') -RedirectStandardError (Join-Path $caseRoot 'worker-error.log')
                $receipt = Wait-FixtureReceipt $plan.status_path
                if (-not $workerProcess.WaitForExit(10000)) { throw 'fixture_worker_did_not_exit' }
            }
            $expected = switch ($scenario) { 'identity_mismatch' {'failed'} 'corrupt_bundle' {'failed'} 'file_occupied' {'rolled_back'} 'startup_rollback' {'rolled_back'} 'parent_exit' {'completed'} }
            if ($receipt.phase -ne $expected) { throw ("fixture_unexpected_phase: $scenario $($receipt.phase) $($receipt.detail)") }
            if ($receipt.target_pid -gt 0) { $ownedProcesses.Add([int]$receipt.target_pid) }
            $live = Get-Process -Id $receipt.target_pid -ErrorAction Stop
            Start-Sleep -Milliseconds 1500
            $live.Refresh()
            if ($live.HasExited) { throw 'fixture_gui_died_after_worker_exit' }
            $results.Add([pscustomobject]@{scenario=$scenario;pass=$true;phase=$receipt.phase;gui_survived_worker_exit=$true})
            [void]$live.CloseMainWindow()
            [void]$live.WaitForExit(5000)
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
