param(
    [string]$ServiceName = "RedClawDesktopM07T02Smoke",

    [string]$ServiceDisplayName = "RedClawDesktop M07 T02 Boot Validation",

    [string]$HostServiceBinaryPath = "",

    [string]$OutputJsonPath = "",

    [int]$StartTimeoutSeconds = 30,

    [switch]$InstallIfMissing,

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

function Get-ServiceStartMode {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $escapedName = $Name.Replace("'", "''")
    $serviceInfo = Get-CimInstance -ClassName Win32_Service -Filter "Name='$escapedName'" -ErrorAction Stop
    if ($null -eq $serviceInfo) {
        throw "Unable to query startup mode for service '$Name'."
    }

    $startMode = [string]$serviceInfo.StartMode
    if ([string]::IsNullOrWhiteSpace($startMode)) {
        return "UNKNOWN"
    }

    return $startMode.ToUpperInvariant()
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

$defaultOutputJsonPath = Join-Path $reportRoot "m07-t02-boot-validation.json"
$resolvedOutputJsonPath = Resolve-DefaultPath -Value $OutputJsonPath -Default $defaultOutputJsonPath

if (-not (Test-Path $resolvedBinaryPath)) {
    throw "Host service binary not found: $resolvedBinaryPath"
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedOutputJsonPath) | Out-Null

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
$createdService = $false

$result = [ordered]@{
    passed = $false
    service_name = $ServiceName
    service_binary_path = $resolvedBinaryPath
    install_if_missing = [bool]$InstallIfMissing
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    checks = [ordered]@{
        service_exists_or_created = $false
        startup_mode_auto = $false
        service_started = $false
        prelogin_readiness_proxy = $false
        reboot_validation_required = $true
    }
    details = [ordered]@{
        startup_mode = ""
        service_status_after_start = ""
        reboot_manual_step = "Reboot machine and verify service status before interactive login."
    }
    cleanup = [ordered]@{
        service_removed = $false
    }
    error = ""
}

try {
    if ($null -eq $service) {
        if (-not $InstallIfMissing) {
            throw "Service '$ServiceName' does not exist. Re-run with -InstallIfMissing or provide an existing service name."
        }

        if (-not (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)) {
            $newServiceArgs = @{
                Name = $ServiceName
                DisplayName = $ServiceDisplayName
                BinaryPathName = $resolvedBinaryPath
                StartupType = "Automatic"
                ErrorAction = "Stop"
            }
            New-Service @newServiceArgs | Out-Null
        }

        $createdService = $true
    }

    $result.checks.service_exists_or_created = $true

    $startupMode = Get-ServiceStartMode -Name $ServiceName
    $result.details.startup_mode = $startupMode
    if ($startupMode -ne "AUTO" -and $startupMode -ne "AUTOMATIC") {
        throw "Service '$ServiceName' startup mode is '$startupMode'. Expected AUTO/AUTOMATIC for boot-time validation."
    }
    $result.checks.startup_mode_auto = $true

    $serviceBeforeStart = Get-Service -Name $ServiceName -ErrorAction Stop
    if ($serviceBeforeStart.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Running) {
        Start-Service -Name $ServiceName -ErrorAction Stop
    }

    Wait-ServiceState -Name $ServiceName -ExpectedState "Running" -TimeoutSeconds $StartTimeoutSeconds
    $result.checks.service_started = $true

    $runningService = Get-Service -Name $ServiceName -ErrorAction Stop
    $result.details.service_status_after_start = $runningService.Status.ToString()
    $result.checks.prelogin_readiness_proxy = ($runningService.Status.ToString() -eq "Running")

    $result.passed = $true
}
catch {
    $result.error = $_.Exception.Message
    throw
}
finally {
    if ($createdService -and -not $KeepServiceAfterRun) {
        $cleanupService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($null -ne $cleanupService -and $cleanupService.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
            Stop-Service -Name $ServiceName -Force -ErrorAction Stop
            Wait-ServiceState -Name $ServiceName -ExpectedState "Stopped" -TimeoutSeconds 15
        }

        $escapedName = $ServiceName.Replace("'", "''")
        $serviceInfo = Get-CimInstance -ClassName Win32_Service -Filter "Name='$escapedName'" -ErrorAction SilentlyContinue
        if ($null -ne $serviceInfo) {
            $deleteResult = Invoke-CimMethod -InputObject $serviceInfo -MethodName Delete -ErrorAction Stop
            if ($null -ne $deleteResult -and $deleteResult.ReturnValue -ne 0) {
                throw "Failed to delete service '$ServiceName' via CIM. ReturnValue=$($deleteResult.ReturnValue)"
            }
        }

        $result.cleanup.service_removed = $true
    }

    $result.completed_utc = (Get-Date).ToUniversalTime().ToString("o")
    $result | ConvertTo-Json -Depth 6 | Out-File -FilePath $resolvedOutputJsonPath -Encoding utf8

    Write-Host "Validation result file: $resolvedOutputJsonPath"
    if ($result.passed) {
        Write-Host "Result: PASS"
        Write-Host "Manual reboot step still required: $($result.details.reboot_manual_step)"
    }
    else {
        Write-Host "Result: FAIL"
        if (-not [string]::IsNullOrWhiteSpace($result.error)) {
            Write-Host "Error: $($result.error)"
        }
    }
}
