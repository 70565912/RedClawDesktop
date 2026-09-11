param(
    [string]$ServiceName = "RedClawDesktopM03Smoke",

    [string]$ServiceDisplayName = "RedClawDesktop M03 Trust Store Smoke",

    [string]$HostServiceBinaryPath = "",

    [string]$TrustStorePath = "",

    [ValidateSet("clear", "retain")]
    [string]$Fallback = "clear",

    [string]$OutputJsonPath = "",

    [switch]$KeepServiceAfterRun
)

$ErrorActionPreference = "Stop"

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "This script requires Administrator privileges. Start PowerShell as Administrator and retry."
    }
}

function Resolve-DefaultPath {
    param(
        [string]$Value,
        [Parameter(Mandatory = $true)]
        [string]$Default
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $Default
    }

    if ([System.IO.Path]::IsPathRooted($Value)) {
        return $Value
    }

    return Join-Path $repoRoot $Value
}

function Wait-ServiceState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [ValidateSet("Running", "Stopped")]
        [string]$ExpectedState,
        [int]$TimeoutSeconds = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
        if ($null -ne $service -and $service.Status.ToString() -eq $ExpectedState) {
            return
        }

        Start-Sleep -Milliseconds 500
    }

    throw "Timed out waiting for service '$Name' to reach state '$ExpectedState'."
}

function Get-ServiceEnvironmentValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $path = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name"
    if (-not (Test-Path $path)) {
        throw "Service registry key not found: $path"
    }

    $value = (Get-ItemProperty -Path $path -Name Environment -ErrorAction Stop).Environment
    if ($value -is [string]) {
        return @($value)
    }

    return @($value)
}

function Assert-EnvironmentMatches {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Actual,
        [Parameter(Mandatory = $true)]
        [string[]]$Expected,
        [Parameter(Mandatory = $true)]
        [string]$Stage
    )

    $actualJoined = ($Actual -join "`n")
    $expectedJoined = ($Expected -join "`n")
    if ($actualJoined -ne $expectedJoined) {
        throw "Service environment mismatch at stage '$Stage'. Expected '$expectedJoined' but found '$actualJoined'."
    }
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptRoot)
$reportRoot = Join-Path $repoRoot "build/reports"

Assert-Administrator

if ([string]::IsNullOrWhiteSpace($HostServiceBinaryPath)) {
    $defaultCandidates = @(
        (Join-Path $repoRoot "build/ninja-x64/src/Debug/redclaw_host_service.exe"),
        (Join-Path $repoRoot "build/vs2022-x64/src/Debug/redclaw_host_service.exe")
    )
    $resolvedBinaryPath = $defaultCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($resolvedBinaryPath)) {
        $resolvedBinaryPath = $defaultCandidates[0]
    }
}
else {
    $resolvedBinaryPath = Resolve-DefaultPath -Value $HostServiceBinaryPath -Default ""
}

$defaultTrustStorePath = Join-Path $repoRoot "build/reports/m03-smoke-trusted-peer-fingerprints.txt"
$resolvedTrustStorePath = Resolve-DefaultPath -Value $TrustStorePath -Default $defaultTrustStorePath

$defaultOutputJsonPath = Join-Path $reportRoot "m03-trust-store-env-override-smoke.json"
$resolvedOutputJsonPath = Resolve-DefaultPath -Value $OutputJsonPath -Default $defaultOutputJsonPath

if (-not (Test-Path $resolvedBinaryPath)) {
    throw "Host service binary not found: $resolvedBinaryPath"
}

$existingService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($null -ne $existingService) {
    throw "Service '$ServiceName' already exists. Use a unique service name for smoke validation."
}

$createdService = $false
$expectedEnvironment = @(
    "REDCLAW_TRUST_STORE_PATH=$resolvedTrustStorePath",
    "REDCLAW_TRUST_STORE_FALLBACK=$Fallback"
)

$result = [ordered]@{
    passed = $false
    service_name = $ServiceName
    service_binary_path = $resolvedBinaryPath
    trust_store_path = $resolvedTrustStorePath
    fallback = $Fallback
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    checks = [ordered]@{
        created = $false
        environment_written = $false
        first_start = $false
        stop_after_start = $false
        second_start = $false
        persisted_after_restart = $false
    }
    environment_before_restart = @()
    environment_after_restart = @()
    cleanup = [ordered]@{
        service_removed = $false
    }
    error = ""
}

try {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedTrustStorePath) | Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedOutputJsonPath) | Out-Null

    if (-not (Test-Path $resolvedTrustStorePath)) {
        New-Item -ItemType File -Path $resolvedTrustStorePath | Out-Null
    }

    $createArgs = @(
        "create",
        $ServiceName,
        "binPath=",
        "`"$resolvedBinaryPath`"",
        "start=",
        "demand",
        "DisplayName=",
        "`"$ServiceDisplayName`""
    )

    $createOutput = & sc.exe @createArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create temporary service '$ServiceName'. sc.exe output: $createOutput"
    }

    $createdService = $true
    $result.checks.created = $true

    $serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    New-ItemProperty -Path $serviceKey -Name Environment -PropertyType MultiString -Value $expectedEnvironment -Force | Out-Null
    $result.checks.environment_written = $true

    $beforeRestart = Get-ServiceEnvironmentValue -Name $ServiceName
    $result.environment_before_restart = $beforeRestart
    Assert-EnvironmentMatches -Actual $beforeRestart -Expected $expectedEnvironment -Stage "post-write"

    $startOutput = & sc.exe start $ServiceName 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Service failed to start. Ensure HostServiceBinaryPath points to a real service executable. sc.exe output: $startOutput"
    }

    Wait-ServiceState -Name $ServiceName -ExpectedState "Running"
    $result.checks.first_start = $true

    $stopOutput = & sc.exe stop $ServiceName 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Service failed to stop. sc.exe output: $stopOutput"
    }

    Wait-ServiceState -Name $ServiceName -ExpectedState "Stopped"
    $result.checks.stop_after_start = $true

    $restartOutput = & sc.exe start $ServiceName 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Service failed to start on second attempt. sc.exe output: $restartOutput"
    }

    Wait-ServiceState -Name $ServiceName -ExpectedState "Running"
    $result.checks.second_start = $true

    $afterRestart = Get-ServiceEnvironmentValue -Name $ServiceName
    $result.environment_after_restart = $afterRestart
    Assert-EnvironmentMatches -Actual $afterRestart -Expected $expectedEnvironment -Stage "post-restart"

    $result.checks.persisted_after_restart = $true
    $result.passed = $true
}
catch {
    $result.error = $_.Exception.Message
    throw
}
finally {
    if ($createdService) {
        $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($null -ne $service -and $service.Status -ne "Stopped") {
            & sc.exe stop $ServiceName | Out-Null
            Start-Sleep -Seconds 1
        }

        if (-not $KeepServiceAfterRun) {
            & sc.exe delete $ServiceName | Out-Null
            $result.cleanup.service_removed = $true
        }
    }

    $result.completed_utc = (Get-Date).ToUniversalTime().ToString("o")
    $result | ConvertTo-Json -Depth 6 | Out-File -FilePath $resolvedOutputJsonPath -Encoding utf8

    Write-Host "Smoke result file: $resolvedOutputJsonPath"
    if ($result.passed) {
        Write-Host "Result: PASS"
    }
    else {
        Write-Host "Result: FAIL"
        if (-not [string]::IsNullOrWhiteSpace($result.error)) {
            Write-Host "Error: $($result.error)"
        }
    }
}
