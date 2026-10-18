param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')

# An in-process named-pipe fixture verifies the actual native server-PID gate.
# It never stops a process or sends input to a desktop.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading.Tasks;
public sealed class RedClawExitPipeFixture : IDisposable {
    readonly NamedPipeServerStream pipe;
    readonly Task work;
    public string Request = "";
    public RedClawExitPipeFixture(string name) {
        pipe = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
        work = Task.Run(async () => {
            try {
                await pipe.WaitForConnectionAsync();
                using (var reader = new StreamReader(pipe, Encoding.UTF8, false, 4096, true))
                using (var writer = new StreamWriter(pipe, new UTF8Encoding(false), 4096, true)) {
                    Request = await reader.ReadLineAsync();
                    if (Request != null) { writer.AutoFlush = true; await writer.WriteLineAsync("{\"ok\":true}"); }
                }
            } catch (IOException) {} catch (ObjectDisposedException) {}
        });
    }
    public void Finish() { if (!work.Wait(5000)) throw new Exception("fixture_timeout"); }
    public void Dispose() { pipe.Dispose(); work.Wait(5000); }
}
'@
# Production retains the full process-identity assertion. This fixture tests
# pipe ownership and the lifecycle request without querying workstation CIM.
function Assert-UpgradeIdentity {
    param($Expected)
    if ($Expected.pid -ne $PID) { throw 'fixture_identity_mismatch' }
}
$identity = [pscustomobject]@{pid=$PID}
$passed = 0
foreach ($case in @(
    @{args=@('--debug-control-name','unused');expected=''},
    @{args=@('--enable-debug-control');expected='RedClawDesktop.DebugControl.v1'},
    @{args=@('--enable-debug-control','--debug-control-name','RedClaw.Test');expected='RedClaw.Test'})) {
    if ((Get-UpgradeControlName $case.args) -cne $case.expected) { throw 'control_name_case_failed' }
    $passed++
}
$rejected = $false
try { Get-UpgradeControlName @('--enable-debug-control','--debug-control-name') | Out-Null }
catch { $rejected = $_.Exception.Message -eq 'upgrade_control_name_invalid' }
if (-not $rejected) { throw 'missing_control_name_not_rejected' }; $passed++

foreach ($wrongOwner in @($false,$true)) {
    $name = 'RedClaw.ExitFixture.' + [guid]::NewGuid().ToString('N')
    $fixture = [RedClawExitPipeFixture]::new($name)
    try {
        if ($wrongOwner) {
            $rejected = $false
            try { Request-UpgradeControlledExit ([pscustomobject]@{pid=0}) $name }
            catch { $rejected = $_.Exception.Message -eq 'upgrade_control_identity_mismatch' }
            $fixture.Finish()
            if (-not $rejected -or $fixture.Request) { throw 'wrong_owner_received_request' }
        } else {
            Request-UpgradeControlledExit $identity $name
            $fixture.Finish()
            $request = $fixture.Request | ConvertFrom-Json
            if ($request.schema -ne 'redclaw.debug-control.v1' -or $request.action -ne 'exit') { throw 'lifecycle_request_invalid' }
        }
        $passed++
    } finally { $fixture.Dispose() }
}
[pscustomobject]@{suite='runtime_controlled_exit';passed=$passed;failed=0} | ConvertTo-Json -Compress
