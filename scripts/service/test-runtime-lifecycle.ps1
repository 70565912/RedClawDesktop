param(
    [Parameter(Mandatory)][string]$ProcDumpPath,
    [ValidateSet('normal','crash','forced','detach')][string[]]$Mode = @('normal','crash','forced','detach')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$root = Join-Path $repoRoot ('build\reports\lifecycle-selftest-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $root | Out-Null
Protect-UpgradePath $root
$sourcePath = Join-Path $root 'LifecycleFixture.cs'
$fixture = Join-Path $root 'LifecycleFixture.exe'
@'
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
class LifecycleFixture {
    static int Main(string[] a) {
        if (a[0] == "child") {
            while (!File.Exists(a[1])) Thread.Sleep(50);
            return 23;
        }
        var child = Process.Start(new ProcessStartInfo {
            FileName = Process.GetCurrentProcess().MainModule.FileName,
            Arguments = "child \"" + a[1] + "\"", UseShellExecute = false,
            CreateNoWindow = true
        });
        File.WriteAllText(a[3], "{\"app_pid\":" + Process.GetCurrentProcess().Id + ",\"runtime_pid\":" + child.Id + "}");
        while (!File.Exists(a[2])) Thread.Sleep(50);
        if (File.ReadAllText(a[2]) == "crash") Environment.FailFast("RedClaw lifecycle fixture only");
        return 17;
    }
}
'@ | Set-Content -LiteralPath $sourcePath -Encoding UTF8
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $compiler /nologo /target:exe /debug:pdbonly ('/out:' + $fixture) $sourcePath
if ($LASTEXITCODE -ne 0) { throw 'lifecycle_fixture_compile_failed' }
$shell = Join-Path $PSHOME 'pwsh.exe'
if (-not (Test-Path -LiteralPath $shell)) { $shell = Join-Path $PSHOME 'powershell.exe' }
$results = @()

function Read-FixtureJson {
    param([string]$Path)
    $stream = [IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() | ConvertFrom-Json }
    finally { $reader.Dispose(); $stream.Dispose() }
}

function Wait-FixtureCondition {
    param([scriptblock]$Condition, [string]$Label, [int]$TimeoutSeconds=45)
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 250
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw ('lifecycle_fixture_timeout_' + $Label)
}

foreach ($caseMode in $Mode) {
    $directory = Join-Path $root $caseMode
    New-Item -ItemType Directory -Path $directory | Out-Null
    Protect-UpgradePath $directory
    $childGate = Join-Path $directory 'child.gate'
    $guiGate = Join-Path $directory 'gui.gate'
    $status = Join-Path $directory 'fixture-status.json'
    $statePath = Join-Path $directory 'monitor-state.json'
    $arguments = @('gui',$childGate,$guiGate,$status)
    $gui = Start-Process -FilePath $fixture -ArgumentList (($arguments | ForEach-Object { ConvertTo-UpgradeArgument $_ }) -join ' ') -PassThru -WindowStyle Hidden
    $watcher = $null
    $runtimeProcess = $null
    try {
        Wait-FixtureCondition { Test-Path -LiteralPath $status } 'status'
        $fixtureStatus = Read-FixtureJson $status
        $fixtureStatus | Add-Member -NotePropertyMembers @{
            updated_at_unix_ms=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); phase='fixture'; connected=$false
            channel_open=$false; dht_reachable=$false
            dht=[pscustomobject]@{publish_success=0;publish_revision=0;publish_expiry=0;publish_expired_total=0;fetch_hits=0;remote_revision=0}
            stream=[pscustomobject]@{captured=0;encoded=0;transmitted=0;synthetic=0;capture_failures=0;encode_failures=0;transmit_failures=0}
        }
        $fixtureStatus | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $status -Encoding UTF8
        $runtimeProcess = [Diagnostics.Process]::GetProcessById($fixtureStatus.runtime_pid)
        $null = $runtimeProcess.Handle
        $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'watch-runtime-lifecycle.ps1'),'-GuiPid',"$($gui.Id)",'-StatusPath',$status,'-ExpectedExecutable',$fixture,'-EvidenceDirectory',$directory,'-ProcDumpPath',$ProcDumpPath)
        $watcher = Start-Process -FilePath $shell -ArgumentList (($arguments | ForEach-Object { ConvertTo-UpgradeArgument $_ }) -join ' ') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $directory 'watcher.stdout.log') -RedirectStandardError (Join-Path $directory 'watcher.stderr.log')
        Wait-FixtureCondition {
            if (-not (Test-Path -LiteralPath $statePath)) { return $false }
            $s = Read-FixtureJson $statePath
            if ($s.phase -eq 'failed') { throw $s.failure }
            return @($s.targets).Count -eq 2 -and $s.collectors_ready
        } 'collectors'
        if ($caseMode -eq 'detach') {
            [IO.File]::WriteAllText((Join-Path $directory 'stop-monitor.request'),'stop')
            Wait-FixtureCondition { $watcher.WaitForExit(0) } 'detach'
            $r = Read-FixtureJson (Join-Path $directory 'monitor-result.json')
            if ($r.phase -ne 'stopped_by_request' -or $gui.HasExited -or $runtimeProcess.HasExited) { throw 'lifecycle_detach_changed_target_lifetime' }
            [IO.File]::WriteAllText($childGate,'exit')
            [IO.File]::WriteAllText($guiGate,'exit')
            Wait-FixtureCondition { $gui.WaitForExit(0) -and $runtimeProcess.WaitForExit(0) } 'detached_fixture_exit'
            if ($gui.ExitCode -ne 17 -or $runtimeProcess.ExitCode -ne 23) { throw 'lifecycle_detached_fixture_exit_mismatch' }
            $results += [pscustomobject]@{mode=$caseMode;passed=$true;targets_resumed=$true;gui_exit_code=$gui.ExitCode;runtime_exit_code=$runtimeProcess.ExitCode}
            continue
        } elseif ($caseMode -eq 'forced') {
            # Only the two exact fixture identities created in this iteration.
            $gui.Kill()
            $runtimeProcess.Kill()
        } elseif ($caseMode -eq 'crash') {
            [IO.File]::WriteAllText($guiGate,'crash')
            Wait-FixtureCondition { $gui.WaitForExit(0) } 'crash_exit'
            if ($watcher.HasExited) { throw 'lifecycle_monitor_died_with_gui' }
            [IO.File]::WriteAllText($childGate,'exit')
        } else {
            [IO.File]::WriteAllText($childGate,'exit')
            Wait-FixtureCondition {
                $s = Read-FixtureJson $statePath
                return @($s.targets | Where-Object { $_.role -eq 'runtime' -and $_.exited }).Count -eq 1
            } 'child_exit'
            if ($watcher.HasExited -or $gui.HasExited) { throw 'lifecycle_monitor_did_not_survive_child_exit' }
            [IO.File]::WriteAllText($guiGate,'exit')
        }
        Wait-FixtureCondition { $watcher.WaitForExit(0) } 'watcher_exit'
        $r = Read-FixtureJson (Join-Path $directory 'monitor-result.json')
        if ($r.phase -ne 'completed' -or $watcher.ExitCode -ne 0) { throw ('lifecycle_monitor_failed_' + $r.failure) }
        if (@($r.targets | Where-Object { $_.exited -and $null -ne $_.exit_code_hex }).Count -ne 2) { throw 'lifecycle_exit_code_missing' }
        $guiResult = $r.targets | Where-Object { $_.role -eq 'gui' }
        $childResult = $r.targets | Where-Object { $_.role -eq 'runtime' }
        if ($caseMode -eq 'normal' -and ($guiResult.exit_code_signed -ne 17 -or $childResult.exit_code_signed -ne 23)) { throw 'lifecycle_normal_exit_code_mismatch' }
        if ($caseMode -eq 'crash' -and $guiResult.exit_code_signed -eq 17) { throw 'lifecycle_crash_exit_not_observed' }
        $dumps = @(Get-ChildItem -LiteralPath $directory -Recurse -File -Filter '*.dmp')
        if ($dumps.Count -lt 2) { throw 'lifecycle_expected_dumps_missing' }
        foreach ($dump in $dumps) {
            $stream = [IO.File]::OpenRead($dump.FullName)
            try { $magic = New-Object byte[] 4; [void]$stream.Read($magic,0,4); if ([Text.Encoding]::ASCII.GetString($magic) -ne 'MDMP') { throw 'lifecycle_dump_header_invalid' } }
            finally { $stream.Dispose() }
        }
        $results += [pscustomobject]@{mode=$caseMode;passed=$true;gui_exit_hex=$guiResult.exit_code_hex;runtime_exit_hex=$childResult.exit_code_hex;dump_count=$dumps.Count;monitor_survived_target_exit=$true}
    } finally {
        # Signal the owned observer to detach before fixture-only cleanup.
        if ($watcher -and -not $watcher.HasExited) {
            [IO.File]::WriteAllText((Join-Path $directory 'stop-monitor.request'),'stop')
            [void]$watcher.WaitForExit(40000)
        }
        foreach ($process in @($gui,$runtimeProcess)) { if ($process -and -not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(5000) } }
    }
}
$result = [ordered]@{schema='redclaw.process-lifecycle.selftest.v1';passed=$true;cases=$results}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'result.json') -Encoding UTF8
$result | ConvertTo-Json -Depth 5
