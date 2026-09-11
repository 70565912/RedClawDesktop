param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('host', 'controller')]
    [string]$Role,

    [Parameter(Mandatory = $true)]
    [string]$SignalDirectory,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_.-]{1,128}$')]
    [string]$BridgeId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_.-]{1,128}$')]
    [string]$SessionEpoch,

    [Parameter(Mandatory = $true)]
    [string]$PassphraseFile,

    [string]$BindAddress = '',

    [string[]]$IceServer = @(
        'stun:stun.douyucdn.cn:18000',
        'stun:stun.l.google.com:19302',
        'stun:stun.cloudflare.com:3478'
    ),

    [ValidatePattern('^[A-Za-z0-9_.-]{0,128}$')]
    [string]$NetworkFingerprint = '',

    [string]$RouteCache = '',

    [string]$ControlName = 'RedClawDesktop.DebugBridge.v1',

    [string]$RedClawControlName = 'RedClawDesktop.DebugControl.v1',

    [switch]$AllowAgentTasks,

    [string]$AgentProjectManifest = '',

    [ValidateRange(0, 604800)]
    [int]$RunSeconds = 0,

    [string]$Executable = '',

    [string]$ReportDirectory = '',

    [switch]$Wait
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $repoRoot 'build\vs2022-x64\src\Debug\redclaw_debug_bridge.exe'
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Debug bridge executable not found: $Executable"
}
if (-not (Test-Path -LiteralPath $PassphraseFile -PathType Leaf)) {
    throw "Passphrase file not found: $PassphraseFile"
}
$passphraseLength = (Get-Content -LiteralPath $PassphraseFile -Raw).TrimEnd("`r", "`n").Length
if ($passphraseLength -lt 16 -or $passphraseLength -gt 1024) {
    throw 'Passphrase file must contain 16..1024 characters.'
}
foreach ($server in $IceServer) {
    if (-not $server.StartsWith('stun:', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Only STUN servers are accepted by the P2P debug bridge: $server"
    }
}
if ($AllowAgentTasks -and $Role -eq 'host' -and [string]::IsNullOrWhiteSpace($AgentProjectManifest)) {
    throw 'Host Agent authorization requires -AgentProjectManifest.'
}

if ([string]::IsNullOrWhiteSpace($RouteCache)) {
    $cacheRoot = Join-Path $env:LOCALAPPDATA 'RedClaw\DebugBridge'
    $RouteCache = Join-Path $cacheRoot ("route-cache-{0}.bin" -f $Role)
}
if ([string]::IsNullOrWhiteSpace($ReportDirectory)) {
    $ReportDirectory = Join-Path $repoRoot ("build\reports\debug-bridge-{0}-{1}" -f $Role, $SessionEpoch)
}
New-Item -ItemType Directory -Force -Path $SignalDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null

$arguments = @(
    '--role', $Role,
    '--signal-dir', (Resolve-Path -LiteralPath $SignalDirectory).Path,
    '--passphrase-file', (Resolve-Path -LiteralPath $PassphraseFile).Path,
    '--bridge-id', $BridgeId,
    '--session-epoch', $SessionEpoch,
    '--route-cache', $RouteCache,
    '--control-name', $ControlName,
    '--redclaw-control-name', $RedClawControlName,
    '--run-seconds', [string]$RunSeconds
)
if (-not [string]::IsNullOrWhiteSpace($BindAddress)) {
    $arguments += @('--bind-address', $BindAddress)
}
if (-not [string]::IsNullOrWhiteSpace($NetworkFingerprint)) {
    $arguments += @('--network-fingerprint', $NetworkFingerprint)
}
foreach ($server in $IceServer) {
    $arguments += @('--ice-server', $server)
}
if ($AllowAgentTasks) {
    $arguments += '--allow-agent-tasks'
    if (-not [string]::IsNullOrWhiteSpace($AgentProjectManifest)) {
        $arguments += @('--agent-project-manifest', (Resolve-Path -LiteralPath $AgentProjectManifest).Path)
    }
}

$stdoutPath = Join-Path $ReportDirectory 'bridge.stdout.log'
$stderrPath = Join-Path $ReportDirectory 'bridge.stderr.log'
$process = Start-Process `
    -FilePath $Executable `
    -ArgumentList $arguments `
    -WorkingDirectory $repoRoot `
    -RedirectStandardOutput $stdoutPath `
    -RedirectStandardError $stderrPath `
    -WindowStyle Hidden `
    -PassThru

$result = [ordered]@{
    schema = 'redclaw.debug-bridge-launch.v1'
    role = $Role
    pid = $process.Id
    bridge_id = $BridgeId
    session_epoch = $SessionEpoch
    control_name = $ControlName
    signal_directory = (Resolve-Path -LiteralPath $SignalDirectory).Path
    route_cache_enabled = -not [string]::IsNullOrWhiteSpace($NetworkFingerprint)
    stdout_log = $stdoutPath
    stderr_log = $stderrPath
}
$result | ConvertTo-Json -Depth 5

if ($Wait) {
    $process.WaitForExit()
    exit $process.ExitCode
}
