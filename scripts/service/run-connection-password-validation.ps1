param(
    [string]$RuntimeExe = 'release\Debug\redclaw_desktop.exe',
    [Parameter(Mandatory = $true)][string]$LegacyRuntimeExe,
    [string]$ReportRoot = 'build\reports\connection-password',
    [ValidateRange(30, 300)][int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$runtime = (Resolve-Path -LiteralPath $RuntimeExe).Path
$legacy = (Resolve-Path -LiteralPath $LegacyRuntimeExe).Path
$runDirectory = [System.IO.Path]::GetFullPath((Join-Path $ReportRoot ('matrix-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))))
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
$owned = [System.Collections.Generic.List[object]]::new()
$results = [System.Collections.Generic.List[object]]::new()
$passwordText = [guid]::NewGuid().ToString('N')
$password = [Security.SecureString]::new()
foreach ($character in $passwordText.ToCharArray()) { $password.AppendChar($character) }
$wrong = [Security.SecureString]::new()
foreach ($character in [guid]::NewGuid().ToString('N').ToCharArray()) { $wrong.AppendChar($character) }
$script:caseTimeoutSeconds = $TimeoutSeconds
$credentialPaths = @{}
foreach ($entry in @(@('host','host',$password), @('correct','controller',$password), @('wrong','controller',$wrong))) {
    $path = Join-Path $runDirectory ($entry[0] + '.dpapi')
    & (Join-Path $PSScriptRoot 'new-connection-credential.ps1') -Role $entry[1] -Password $entry[2] -OutputPath $path | Out-Null
    $credentialPaths[$entry[0]] = $path
}
function Start-TestRuntime {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseShouldProcessForStateChangingFunctions', '', Justification='This bounded test starts only its explicitly requested test-owned processes.')]
    [CmdletBinding()]
    param([string]$Name, [string]$Role, [string]$Exe, [string]$ProtectedFile, [string]$Code, [string]$Transport, [string]$Signal, [int]$Port)
    $directory = Join-Path $runDirectory $Name
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $arguments = @('--cli','--role',$Role,'--signal-transport',$Transport,'--session-code',$Code,
        '--signal-dir',$Signal,'--signal-timeout-seconds','0','--run-seconds','0','--stream-smoke',
        '--ice-udp-port',[string]$Port,'--log-dir',$directory,'--enable-ice-tcp')
    if ($ProtectedFile) { $arguments += @('--connection-credential-file', $ProtectedFile) }
    if ($Role -eq 'host') { $arguments += '--stream-require-capture' }
    $out = Join-Path $directory 'out.log'; $err = Join-Path $directory 'err.log'
    $process = Start-Process -FilePath $Exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $record = [pscustomobject]@{ Process=$process; Directory=$directory; Out=$out; Err=$err }
    $owned.Add($record)
    return $record
}
function Read-TestLog($Record) {
    return ((Get-Content -LiteralPath $Record.Out -Raw -ErrorAction SilentlyContinue) + "`n" +
        (Get-Content -LiteralPath $Record.Err -Raw -ErrorAction SilentlyContinue))
}
function Wait-TestLog($Record, [string]$Pattern) {
    $deadline = [datetime]::UtcNow.AddSeconds($script:caseTimeoutSeconds)
    while ([datetime]::UtcNow -lt $deadline) {
        if ((Read-TestLog $Record) -match $Pattern) { return $true }
        $Record.Process.Refresh()
        if ($Record.Process.HasExited) { return (Read-TestLog $Record) -match $Pattern }
        Start-Sleep -Milliseconds 200
    }
    return $false
}
function Stop-TestRuntime {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseShouldProcessForStateChangingFunctions', '', Justification='Cleanup must stop the exact processes started by this test even after a failed assertion.')]
    [CmdletBinding()]
    param($Record)
    if ($null -eq $Record) { return }
    $Record.Process.Refresh()
    if (-not $Record.Process.HasExited) { $Record.Process.Kill(); $Record.Process.WaitForExit(10000) | Out-Null }
}
try {
    $port = 55400
    foreach ($case in @('wrong-password','new-host-old-client','old-host-new-client')) {
        Write-Host "[connection-password] $case"
        $code = [guid]::NewGuid().ToString('N').Substring(0,8).ToUpperInvariant()
        $signal = Join-Path $runDirectory ($case + '-signal')
        New-Item -ItemType Directory -Path $signal -Force | Out-Null
        $oldHost = $case -eq 'old-host-new-client'; $oldClient = $case -eq 'new-host-old-client'
        $hostExe = if ($oldHost) { $legacy } else { $runtime }
        $clientExe = if ($oldClient) { $legacy } else { $runtime }
        $hostCredential = if ($oldHost) { '' } else { $credentialPaths.host }
        $clientCredential = if ($oldClient) { '' } elseif ($case -eq 'wrong-password') { $credentialPaths.wrong } else { $credentialPaths.correct }
        $hostRecord = Start-TestRuntime "$case-host" host $hostExe $hostCredential $code file $signal $port
        Start-Sleep -Seconds 2
        $clientRecord = Start-TestRuntime "$case-client" controller $clientExe $clientCredential $code file $signal ($port + 1)
        $protected = if ($oldHost) { $clientRecord } else { $hostRecord }
        $expected = if ($case -eq 'wrong-password') { 'connection_password_rejected' } else { 'protocol_version_incompatible' }
        $rejected = Wait-TestLog $protected "connection_auth rejected reason=$expected"
        $log = Read-TestLog $protected
        $leaked = $log -match 'connection_auth accepted|transmitted=[1-9]|received=[1-9]|rendered=[1-9]|decoded=[1-9]'
        $results.Add([pscustomobject]@{case=$case; rejected=$rejected; no_business=$(-not $leaked); passed=$($rejected -and -not $leaked)})
        Stop-TestRuntime $hostRecord; Stop-TestRuntime $clientRecord
        $port += 2
    }
    Write-Host '[connection-password] online Host rejects then accepts next correct Client'
    $code = [guid]::NewGuid().ToString('N').Substring(0,8).ToUpperInvariant()
    $hostRecord = Start-TestRuntime 'recovery-host' host $runtime $credentialPaths.host $code dht (Join-Path $runDirectory 'recovery-host-signal') $port
    Start-Sleep -Seconds 5
    $badRecord = Start-TestRuntime 'recovery-wrong-client' controller $runtime $credentialPaths.wrong $code dht (Join-Path $runDirectory 'recovery-wrong-signal') ($port+1)
    $rejected = Wait-TestLog $badRecord 'connection_auth rejected reason=connection_password_rejected'
    $before = Read-TestLog $hostRecord
    $noBusiness = $before -notmatch 'connection_auth accepted|transmitted=[1-9]|received=[1-9]'
    Stop-TestRuntime $badRecord
    $hostRecord.Process.Refresh(); $sameHost = -not $hostRecord.Process.HasExited
    $correctRecord = Start-TestRuntime 'recovery-correct-client' controller $runtime $credentialPaths.correct $code dht (Join-Path $runDirectory 'recovery-correct-signal') ($port+2)
    $accepted = Wait-TestLog $correctRecord 'connection_auth accepted'
    $frames = $accepted -and (Wait-TestLog $hostRecord 'transmitted=[1-9]')
    $results.Add([pscustomobject]@{case='online-recovery'; wrong_rejected=$rejected; no_business_before_auth=$noBusiness; same_host=$sameHost; correct_accepted=$accepted; real_frames=$frames; passed=$($rejected -and $noBusiness -and $sameHost -and $frames)})
} finally {
    foreach ($record in $owned) { Stop-TestRuntime $record }
    $password.Dispose(); $wrong.Dispose()
}
$secretFound = $false
foreach ($file in Get-ChildItem -LiteralPath $runDirectory -Recurse -File -Filter '*.log') {
    if ([System.IO.File]::ReadAllText($file.FullName).Contains($passwordText)) { $secretFound = $true }
}
$passwordText = $null
$passed = @($results | Where-Object { -not $_.passed }).Count -eq 0 -and -not $secretFound
[pscustomobject]@{version=1; passed=$passed; plaintext_in_logs=$secretFound; cases=$results.ToArray(); runtime_sha256=(Get-FileHash $runtime).Hash; legacy_sha256=(Get-FileHash $legacy).Hash} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runDirectory 'result.json') -Encoding utf8
Write-Host "[connection-password] passed=$passed report=$runDirectory"
if (-not $passed) { exit 1 }
