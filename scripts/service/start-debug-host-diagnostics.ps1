param(
    [string]$ProcDumpPath = '',
    [string]$EvidenceDirectory = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if ([string]::IsNullOrWhiteSpace($ProcDumpPath)) { $ProcDumpPath = Join-Path $repoRoot 'build\tools\procdump\procdump64.exe' }
$ProcDumpPath = (Resolve-Path -LiteralPath $ProcDumpPath).Path
$signature = Get-AuthenticodeSignature -LiteralPath $ProcDumpPath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') { throw 'lifecycle_procdump_signature_invalid' }
if (@(Get-Process -Name redclaw_desktop -ErrorAction SilentlyContinue).Count) { throw 'A RedClaw instance is already running; no process was stopped.' }
if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) { $EvidenceDirectory = Join-Path $repoRoot ('build\reports\host-lifecycle-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
if (Test-Path -LiteralPath $EvidenceDirectory) { throw 'Use a new diagnostic evidence directory.' }
New-Item -ItemType Directory -Path $EvidenceDirectory | Out-Null
Protect-UpgradePath $EvidenceDirectory
$EvidenceDirectory = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$taskName = 'RedClaw-Diagnostics-' + [guid]::NewGuid().ToString('N')
$worker = Join-Path $PSScriptRoot 'watch-runtime-lifecycle.ps1'
$shell = Join-Path $PSHOME 'pwsh.exe'
if (-not (Test-Path -LiteralPath $shell)) { $shell = Join-Path $PSHOME 'powershell.exe' }
$arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',$worker,'-EvidenceDirectory',$EvidenceDirectory,'-ProcDumpPath',$ProcDumpPath,'-TaskName',$taskName)
$action = New-ScheduledTaskAction -Execute $shell -Argument (($arguments | ForEach-Object { ConvertTo-UpgradeArgument $_ }) -join ' ') -WorkingDirectory $repoRoot
$principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $taskName
$receipt = [ordered]@{schema='redclaw.host-diagnostics.dispatch.v1';task_name=$taskName;evidence_directory=$EvidenceDirectory;state_path=(Join-Path $EvidenceDirectory 'monitor-state.json');dump_type='mini';trigger='unhandled_exception_or_termination';automatic_restart=$false}
$receipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'dispatch.json') -Encoding UTF8
$receipt | ConvertTo-Json
