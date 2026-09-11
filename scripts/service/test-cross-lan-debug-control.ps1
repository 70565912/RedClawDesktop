param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$RuntimeExe = '',

    [string]$RunId = '',

    [switch]$KeepSupervisor
)

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

function Invoke-RawDebugControl {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ControlName,

        [Parameter(Mandatory = $true)]
        [hashtable]$Request,

        [int]$TimeoutMs = 5000
    )

    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.',
        $ControlName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    try {
        $pipe.Connect($TimeoutMs)
        $encoding = [System.Text.UTF8Encoding]::new($false)
        $writer = [System.IO.StreamWriter]::new($pipe, $encoding, 4096, $true)
        $reader = [System.IO.StreamReader]::new($pipe, $encoding, $false, 4096, $true)
        try {
            $writer.AutoFlush = $true
            $writer.WriteLine(($Request | ConvertTo-Json -Depth 8 -Compress))
            $line = $reader.ReadLine()
            if ([string]::IsNullOrWhiteSpace($line)) {
                throw 'Debug control returned no response.'
            }
            return ($line | ConvertFrom-Json)
        } finally {
            $reader.Dispose()
            $writer.Dispose()
        }
    } finally {
        $pipe.Dispose()
    }
}

function New-DebugControlRequest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Action
    )

    return @{
        schema = 'redclaw.debug-control.v1'
        request_id = [guid]::NewGuid().ToString('N')
        action = $Action
    }
}

function Wait-DebugStatus {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ControlName,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Predicate,

        [int]$TimeoutSeconds = 20
    )

    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $response = Invoke-RawDebugControl -ControlName $ControlName -Request (New-DebugControlRequest -Action 'status')
        if ([bool]$response.ok -and (& $Predicate $response.status)) {
            return $response.status
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTimeOffset]::UtcNow -lt $deadline)

    throw ("Timed out waiting for debug status. Last phase={0} running={1}" -f `
        $response.status.phase, $response.status.runtime_running)
}

function Test-EvidenceManifest {
    param([string]$ManifestPath)

    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 'redclaw.debug.evidence.manifest.v1') {
        throw 'Evidence manifest has an unexpected schema.'
    }
    foreach ($file in @($manifest.files)) {
        if (-not (Test-Path -LiteralPath $file.path -PathType Leaf)) {
            throw "Evidence snapshot is missing: $($file.path)"
        }
        $actual = (Get-FileHash -LiteralPath $file.path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne ([string]$file.sha256).ToLowerInvariant()) {
            throw "Evidence snapshot hash mismatch: $($file.path)"
        }
    }
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot
if ([string]::IsNullOrWhiteSpace($RuntimeExe)) {
    $RuntimeExe = Join-Path $repoRoot ("release\{0}\redclaw_desktop.exe" -f $Configuration)
}
if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = 'controltest_' + (Get-Date -Format 'yyyyMMdd_HHmmss')
}
$controlName = 'RedClawDesktop.DebugControl.Test.' + [guid]::NewGuid().ToString('N')

$existing = Get-Process -Name 'redclaw_desktop' -ErrorAction SilentlyContinue
if (@($existing).Count -gt 0) {
    throw 'Debug-control integration test requires no existing RedClawDesktop process.'
}

$supervisorResult = $null
try {
    $supervisorOutput = & (Join-Path $PSScriptRoot 'start-cross-lan-debug-supervisor.ps1') `
        -Role host `
        -Configuration $Configuration `
        -RuntimeExe $RuntimeExe `
        -RunId $RunId `
        -ControlName $controlName `
        -IceServer 'stun:stun.l.google.com:19302' `
        -SkipPrep
    if ($LASTEXITCODE -ne 0) {
        throw "Supervisor script failed with exit code $LASTEXITCODE"
    }
    $supervisorResult = (($supervisorOutput | Out-String).Trim() | ConvertFrom-Json)

    $clientOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot 'invoke-cross-lan-debug-control.ps1') `
        -Action status -ControlName $controlName -Json
    if ($LASTEXITCODE -ne 0) {
        throw "Debug control client status failed with exit code $LASTEXITCODE"
    }
    $idle = (($clientOutput | Out-String).Trim() | ConvertFrom-Json)
    if (-not [bool]$idle.ok -or $idle.status.phase -ne 'idle' -or [bool]$idle.status.runtime_running) {
        throw 'Supervisor did not start in the expected idle state.'
    }

    $wrongPipeRejected = $false
    try {
        Invoke-RawDebugControl `
            -ControlName ($controlName + '.missing') `
            -Request (New-DebugControlRequest -Action 'status') `
            -TimeoutMs 200 | Out-Null
    } catch {
        $wrongPipeRejected = $true
    }
    if (-not $wrongPipeRejected) {
        throw 'A nonexistent debug-control pipe unexpectedly accepted a connection.'
    }

    $unsupported = New-DebugControlRequest -Action 'status'
    $unsupported.output_path = 'C:\not-allowed'
    $unsupportedResponse = Invoke-RawDebugControl -ControlName $controlName -Request $unsupported
    if ([bool]$unsupportedResponse.ok -or $unsupportedResponse.error_code -ne 'unsupported_field') {
        throw 'Arbitrary output_path field was not rejected.'
    }

    $wrongRole = New-DebugControlRequest -Action 'start'
    $wrongRole.role = 'controller'
    $wrongRoleResponse = Invoke-RawDebugControl -ControlName $controlName -Request $wrongRole
    if ([bool]$wrongRoleResponse.ok -or $wrongRoleResponse.error_code -ne 'role_mismatch') {
        throw 'A role that did not match the supervisor configuration was not rejected.'
    }

    $start = New-DebugControlRequest -Action 'start'
    $start.role = 'host'
    $startResponse = Invoke-RawDebugControl -ControlName $controlName -Request $start
    if (-not [bool]$startResponse.ok) {
        throw "Host start failed: $($startResponse.error_code) $($startResponse.error_detail)"
    }
    $runningStatus = Wait-DebugStatus -ControlName $controlName -Predicate {
        param($status)
        return [bool]$status.runtime_running -and [int64]$status.runtime_pid -gt 0
    }
    $firstRuntimePid = [int64]$runningStatus.runtime_pid

    $duplicateStart = Invoke-RawDebugControl -ControlName $controlName -Request $start
    if ([bool]$duplicateStart.ok -or $duplicateStart.error_code -ne 'already_running') {
        throw 'Duplicate start did not return already_running.'
    }

    $reconnectResponse = Invoke-RawDebugControl `
        -ControlName $controlName `
        -Request (New-DebugControlRequest -Action 'reconnect')
    if (-not [bool]$reconnectResponse.ok) {
        throw "Reconnect failed: $($reconnectResponse.error_code)"
    }
    $reconnectedStatus = Wait-DebugStatus -ControlName $controlName -Predicate {
        param($status)
        return [bool]$status.runtime_running -and [int64]$status.runtime_pid -gt 0
    }
    if ([int64]$reconnectedStatus.runtime_pid -eq 0 -or $firstRuntimePid -eq 0) {
        throw 'Reconnect did not expose a valid runtime PID.'
    }

    $tailRequest = New-DebugControlRequest -Action 'tail_log'
    $tailRequest.limit = 20
    $tailResponse = Invoke-RawDebugControl -ControlName $controlName -Request $tailRequest
    if (-not [bool]$tailResponse.ok -or @($tailResponse.result.lines).Count -gt 20) {
        throw 'tail_log returned an invalid response.'
    }

    $stopResponse = Invoke-RawDebugControl `
        -ControlName $controlName `
        -Request (New-DebugControlRequest -Action 'stop')
    if (-not [bool]$stopResponse.ok) {
        throw "Stop failed: $($stopResponse.error_code)"
    }
    Wait-DebugStatus -ControlName $controlName -Predicate {
        param($status)
        return -not [bool]$status.runtime_running
    } | Out-Null

    $notRunningReconnect = Invoke-RawDebugControl `
        -ControlName $controlName `
        -Request (New-DebugControlRequest -Action 'reconnect')
    if ([bool]$notRunningReconnect.ok -or $notRunningReconnect.error_code -ne 'not_running') {
        throw 'Reconnect without an active runtime did not return not_running.'
    }

    $exportResponse = Invoke-RawDebugControl `
        -ControlName $controlName `
        -Request (New-DebugControlRequest -Action 'export_evidence')
    if (-not [bool]$exportResponse.ok) {
        throw "Evidence export failed: $($exportResponse.error_code) $($exportResponse.error_detail)"
    }
    Test-EvidenceManifest -ManifestPath $exportResponse.result.manifest_path
    $manifestHash = (Get-FileHash -LiteralPath $exportResponse.result.manifest_path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($manifestHash -ne ([string]$exportResponse.result.manifest_sha256).ToLowerInvariant()) {
        throw 'Evidence manifest response hash does not match the exported file.'
    }

    Write-Host ("[debug-control-test] PASS run_id={0} app_pid={1} manifest={2}" -f `
        $RunId, $supervisorResult.app_pid, $exportResponse.result.manifest_path)
} finally {
    if (-not $KeepSupervisor -and $null -ne $supervisorResult -and [int]$supervisorResult.app_pid -gt 0) {
        $ownedProcess = Get-Process -Id ([int]$supervisorResult.app_pid) -ErrorAction SilentlyContinue
        if ($null -ne $ownedProcess) {
            Stop-Process -Id $ownedProcess.Id
            $ownedProcess.WaitForExit(5000) | Out-Null
        }
    }
}
