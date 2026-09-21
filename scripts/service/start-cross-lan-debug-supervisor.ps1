param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('host', 'controller')]
    [string]$Role,

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$RuntimeExe = '',

    [string]$ConnectionCredentialFile = '',

    [string]$SessionCode = '',

    [string]$RunId = '',

    [string]$ReportRoot = 'build\reports',

    [string[]]$IceServer = @(),

    [string]$IceServerFile = '',

    [string]$ControlName = 'RedClawDesktop.DebugControl.v1',

    [string]$AgentControlName = 'RedClawDesktop.AgentControl.v1',

    [string]$CoordinationJournalPath = '',

    [switch]$AllowRemoteAgent,

    [string[]]$AgentProjectRoot = @(),

    [ValidateRange(0, 3600)]
    [int]$SignalTimeoutSeconds = 0,

    [ValidateRange(0, 65535)]
    [int]$DhtListenPort = 0,

    [ValidateRange(1, 65535)]
    [int]$IceUdpPort = 55000,

    [string]$NetworkBindAddress = '',

    [switch]$AutoStart,

    [switch]$InputDiagnostics,

    [switch]$SkipPrep,

    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

if ($AllowRemoteAgent -and $AgentProjectRoot.Count -eq 0) {
    throw 'AllowRemoteAgent requires at least one explicit AgentProjectRoot before startup.'
}

. (Join-Path $PSScriptRoot 'cross-lan-integration-config.ps1')

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

function Get-RedactedSessionCode {
    param([string]$Code)
    if ([string]::IsNullOrWhiteSpace($Code) -or $Code.Length -lt 4) {
        return '<redacted>'
    }
    return ($Code.Substring(0, 4) + '****')
}

function Protect-IceServerFile {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }
    $resolved = Resolve-Path -LiteralPath $Path
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $acl = [System.Security.AccessControl.FileSecurity]::new()
    $acl.SetOwner($identity.User)
    $acl.SetAccessRuleProtection($true, $false)
    $rule = [System.Security.AccessControl.FileSystemAccessRule]::new(
        $identity.User,
        [System.Security.AccessControl.FileSystemRights]::Read,
        [System.Security.AccessControl.AccessControlType]::Allow)
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $resolved.Path -AclObject $acl
}

function Get-IceServerFileEntryCount {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return 0
    }
    return @(
        Get-Content -LiteralPath $Path |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and -not $_.TrimStart().StartsWith('#') }
    ).Count
}

function ConvertTo-CommandLineArgument {
    param([string]$Value)

    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return ('"{0}"' -f ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1'))
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

if ([string]::IsNullOrWhiteSpace($SessionCode)) {
    $SessionCode = $script:CrossLanIntegrationSessionCode
}
if ($SessionCode -notmatch '^[A-Za-z0-9]{8}$') {
    throw 'SessionCode must contain exactly 8 letters or digits.'
}
if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = Get-Date -Format 'yyyyMMdd_HHmmss'
}
if ($RunId -notmatch '^[A-Za-z0-9._-]{1,64}$') {
    throw 'RunId may contain only letters, digits, dot, dash, and underscore.'
}
if (-not [string]::IsNullOrWhiteSpace($NetworkBindAddress)) {
    $parsedNetworkAddress = $null
    if (-not [System.Net.IPAddress]::TryParse($NetworkBindAddress, [ref]$parsedNetworkAddress) -or
        $parsedNetworkAddress.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
        throw 'NetworkBindAddress must be an IPv4 address assigned to the selected local network exit.'
    }
}
if ([string]::IsNullOrWhiteSpace($RuntimeExe)) {
    $RuntimeExe = Join-Path $repoRoot ("release\{0}\redclaw_desktop.exe" -f $Configuration)
}
$runtimePath = (Resolve-Path -LiteralPath $RuntimeExe).Path
if ([string]::IsNullOrWhiteSpace($CoordinationJournalPath)) {
    $CoordinationJournalPath = Join-Path $env:LOCALAPPDATA 'RedClawDesktop\coordination\coordination-v1.jsonl'
}

if ($IceServer.Count -eq 0) {
    $IceServer = @($script:CrossLanIntegrationIceServers)
}
if ([string]::IsNullOrWhiteSpace($IceServerFile)) {
    $candidate = Join-Path $env:LOCALAPPDATA 'RedClawDesktop\config\ice-servers.local.conf'
    if (Test-Path -LiteralPath $candidate) {
        $IceServerFile = $candidate
    }
}
if (-not [string]::IsNullOrWhiteSpace($IceServerFile)) {
    $IceServerFile = (Resolve-Path -LiteralPath $IceServerFile).Path
}

$iceServerFileEntryCount = Get-IceServerFileEntryCount -Path $IceServerFile

$reportRootAbsolute = if ([System.IO.Path]::IsPathRooted($ReportRoot)) {
    $ReportRoot
} else {
    Join-Path $repoRoot $ReportRoot
}
$runDirectory = Join-Path $reportRootAbsolute ("cross-lan-gui-{0}\{1}" -f $RunId, $Role)

if ($DryRun) {
    Write-Host ("[debug-supervisor] runtime={0}" -f $runtimePath)
    Write-Host ("[debug-supervisor] run_directory={0}" -f $runDirectory)
    Write-Host ("[debug-supervisor] role={0} session={1} ice_server_count={2} ice_server_file_present={3}" -f `
        $Role, (Get-RedactedSessionCode -Code $SessionCode), (@($IceServer).Count + $iceServerFileEntryCount), `
        (-not [string]::IsNullOrWhiteSpace($IceServerFile)))
    Write-Host ("[debug-supervisor] network_bind_address={0}" -f `
        $(if ([string]::IsNullOrWhiteSpace($NetworkBindAddress)) { 'auto' } else { $NetworkBindAddress }))
    Write-Host ("[debug-supervisor] ice_udp_port={0} upnp=enabled" -f $IceUdpPort)
    Write-Host '[debug-supervisor] signal_transport=dht'
    Write-Host ("[debug-supervisor] agent_control={0}" -f $AgentControlName)
    exit 0
}

New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null

if (-not [string]::IsNullOrWhiteSpace($IceServerFile)) {
    Protect-IceServerFile -Path $IceServerFile
}

if (-not $SkipPrep) {
    & (Join-Path $PSScriptRoot 'prepare-dht-remote-validation.ps1') `
        -Role both -Configuration $Configuration -SessionCode $SessionCode | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "prepare-dht-remote-validation.ps1 failed with exit code $LASTEXITCODE"
    }
}

$existing = Get-Process -Name 'redclaw_desktop' -ErrorAction SilentlyContinue
if (@($existing).Count -gt 0) {
    throw ('A RedClawDesktop process is already running. Refusing to start a second supervisor. PIDs: ' + (($existing.Id) -join ','))
}

$gitSha = (& git rev-parse HEAD).Trim()
$runtimeHash = (Get-FileHash -LiteralPath $runtimePath -Algorithm SHA256).Hash.ToLowerInvariant()
$runtimeFile = Get-Item -LiteralPath $runtimePath
$manifestPath = Join-Path $runDirectory 'run-manifest.json'
$manifest = [pscustomobject]@{
    schema = 'redclaw.cross-lan.gui.run.v1'
    generated_at = (Get-Date).ToString('o')
    run_id = $RunId
    role = $Role
    configuration = $Configuration
    session_code_redacted = Get-RedactedSessionCode -Code $SessionCode
    git_sha = $gitSha
    runtime_path = $runtimePath
    runtime_sha256 = $runtimeHash
    runtime_last_write_time = $runtimeFile.LastWriteTime.ToString('o')
    control_name = $ControlName
    agent_control_name = $AgentControlName
    coordination_journal_configured = $true
    local_agent_authorization_requested = [bool]$AllowRemoteAgent
    ice_server_count = @($IceServer).Count + $iceServerFileEntryCount
    ice_server_file_present = -not [string]::IsNullOrWhiteSpace($IceServerFile)
    ice_udp_port = $IceUdpPort
    network_bind_address = $(if ([string]::IsNullOrWhiteSpace($NetworkBindAddress)) { 'auto' } else { $NetworkBindAddress })
    signal_transport = 'dht'
    log_directory = $runDirectory
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

$signalDir = if ($Role -eq 'host') { 'runtime-signaling-cross-lan-host' } else { 'runtime-signaling-cross-lan-controller' }
$arguments = @(
    '--gui-role', $Role,
    '--session-code', $SessionCode,
    '--signal-transport', 'dht',
    '--signal-timeout-seconds', "$SignalTimeoutSeconds",
    '--run-seconds', '0',
    '--signal-dir', $signalDir,
    '--stream-smoke',
    '--enable-ice-tcp',
    '--ice-udp-port', "$IceUdpPort",
    '--enable-port-mapping',
    '--log-dir', $runDirectory,
    '--run-id', $RunId,
    '--enable-debug-control',
    '--debug-control-name', $ControlName
)
foreach ($server in $IceServer) {
    $arguments += @('--ice-server', $server)
}
if (-not [string]::IsNullOrWhiteSpace($IceServerFile)) {
    $arguments += @('--ice-server-file', $IceServerFile)
}
if ($Role -eq 'host') {
    $arguments += @('--gui-persist-host-wait', '--stream-require-capture')
}
$arguments += @(
    '--enable-agent-control',
    '--agent-control-name', $AgentControlName,
    '--coordination-journal-path', $CoordinationJournalPath,
    '--coordination-git-sha', $gitSha
)
if ($AllowRemoteAgent) { $arguments += '--allow-remote-agent' }
foreach ($project in $AgentProjectRoot) { $arguments += @('--agent-project-root', $project) }
if ($DhtListenPort -gt 0) {
    $arguments += @('--dht-listen-port', "$DhtListenPort")
}
if (-not [string]::IsNullOrWhiteSpace($NetworkBindAddress)) {
    $arguments += @('--network-bind-address', $NetworkBindAddress)
}
if ($ConnectionCredentialFile) {
    $arguments += @('--connection-credential-file', (Resolve-Path -LiteralPath $ConnectionCredentialFile).Path)
}
if ($AutoStart) {
    $arguments += '--gui-auto-start'
}
if ($InputDiagnostics) {
    if ($Configuration -ne 'Debug') { throw 'Input diagnostics require Debug.' }
    $arguments += '--input-diagnostics'
}

$argumentLine = ($arguments | ForEach-Object { ConvertTo-CommandLineArgument -Value $_ }) -join ' '
$process = Start-Process -FilePath $runtimePath -ArgumentList $argumentLine -PassThru -WindowStyle Normal
$statusPath = Join-Path $runDirectory 'status.json'
$deadline = [DateTimeOffset]::UtcNow.AddSeconds(15)
while (-not (Test-Path -LiteralPath $statusPath) -and [DateTimeOffset]::UtcNow -lt $deadline) {
    if ($process.HasExited) {
        throw "RedClawDesktop supervisor exited early with code $($process.ExitCode)."
    }
    Start-Sleep -Milliseconds 250
}
if (-not (Test-Path -LiteralPath $statusPath)) {
    throw "RedClawDesktop supervisor did not create status.json within 15 seconds."
}

$resultPath = Join-Path $runDirectory 'supervisor-result.json'
$result = [pscustomobject]@{
    schema = 'redclaw.cross-lan.gui.supervisor.result.v1'
    started_at = (Get-Date).ToString('o')
    run_id = $RunId
    role = $Role
    app_pid = $process.Id
    control_name = $ControlName
    run_directory = $runDirectory
    status_path = $statusPath
    manifest_path = $manifestPath
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Depth 6
