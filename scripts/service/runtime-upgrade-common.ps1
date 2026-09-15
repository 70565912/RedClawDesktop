# Shared, fixed runtime-directory upgrade operations. No command queue.
Set-StrictMode -Version Latest

function Protect-UpgradePath {
    param([Parameter(Mandatory)][string]$Path)
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $item = Get-Item -LiteralPath $Path
    # Preserve the existing owner/SACL. Updating just the DACL must not require
    # SeSecurityPrivilege, including on a second validation of the same plan.
    $acl = Get-Acl -LiteralPath $Path
    if ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -ne $identity.User.Value) {
        throw 'upgrade_plan_owner_mismatch'
    }
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($existing in @($acl.Access)) { [void]$acl.RemoveAccessRuleSpecific($existing) }
    $rule = if ($item.PSIsContainer) {
        [Security.AccessControl.FileSystemAccessRule]::new($identity.User, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
    } else { [Security.AccessControl.FileSystemAccessRule]::new($identity.User, 'FullControl', 'Allow') }
    $acl.AddAccessRule($rule)
    if ($PSVersionTable.PSEdition -eq 'Core') {
        [IO.FileSystemAclExtensions]::SetAccessControl($item, $acl)
    } else {
        $item.SetAccessControl($acl)
    }
}

function Assert-UpgradeOwner {
    param([Parameter(Mandatory)][string]$Path)
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $acl = Get-Acl -LiteralPath $Path
    if (-not $acl.AreAccessRulesProtected -or $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -ne $sid) {
        throw 'upgrade_plan_owner_mismatch'
    }
    foreach ($rule in $acl.Access) {
        if ($rule.AccessControlType -eq 'Allow' -and $rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -ne $sid) {
            throw 'upgrade_plan_acl_not_private'
        }
    }
}

function Get-UpgradeDirectory {
    param([Parameter(Mandatory)][string]$Path)
    $item = Get-Item -LiteralPath $Path
    if (-not $item.PSIsContainer -or $item.FullName.TrimEnd('\') -eq [IO.Path]::GetPathRoot($item.FullName).TrimEnd('\')) {
        throw 'upgrade_directory_must_not_be_volume_root'
    }
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'upgrade_directory_reparse_point' }
    return $item.FullName.TrimEnd('\')
}

function Get-UpgradeManifest {
    param([Parameter(Mandatory)][string]$Directory)
    $root = Get-UpgradeDirectory $Directory
    $items = @(Get-ChildItem -LiteralPath $root -Recurse -Force)
    if (@($items | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count) { throw 'upgrade_bundle_reparse_point' }
    return @($items | Where-Object {-not $_.PSIsContainer} | Sort-Object FullName | ForEach-Object {
        [pscustomobject]@{path=$_.FullName.Substring($root.Length + 1); length=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
}

function Assert-UpgradeManifest {
    param([string]$Directory, [object[]]$Manifest)
    $actual = @(Get-UpgradeManifest $Directory)
    if ($actual.Count -ne $Manifest.Count) { throw 'upgrade_bundle_file_count_mismatch' }
    $expected = @{}
    foreach ($entry in $Manifest) {
        if ([IO.Path]::IsPathRooted($entry.path) -or $entry.path -match '(^|[\\/])\.\.([\\/]|$)' -or $expected.ContainsKey($entry.path)) { throw 'upgrade_bundle_invalid_path' }
        $expected[$entry.path] = $entry
    }
    foreach ($entry in $actual) {
        if (-not $expected.ContainsKey($entry.path) -or $entry.sha256 -ne $expected[$entry.path].sha256 -or $entry.length -ne $expected[$entry.path].length) { throw 'upgrade_bundle_hash_mismatch' }
    }
    foreach ($required in @('redclaw_desktop.exe','redclaw_protocol_codec.exe')) {
        if (-not $expected.ContainsKey($required)) { throw ('upgrade_bundle_missing_' + $required) }
    }
    foreach ($module in @('Core','Gui','Widgets')) {
        if (-not ($expected.ContainsKey('Qt6' + $module + '.dll') -or $expected.ContainsKey('Qt6' + $module + 'd.dll'))) { throw ('upgrade_bundle_missing_qt_' + $module) }
    }
    if (-not ($expected.ContainsKey('platforms\qwindows.dll') -or $expected.ContainsKey('platforms\qwindowsd.dll'))) { throw 'upgrade_bundle_missing_qt_platform' }
}

function Get-UpgradeIdentity {
    param([Parameter(Mandatory)][int]$ProcessId)
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId"
    if (-not $process -or -not $process.ExecutablePath -or -not $process.CommandLine) { throw 'upgrade_target_identity_unavailable' }
    $owner = Invoke-CimMethod -InputObject $process -MethodName GetOwnerSid
    if ($owner.ReturnValue -ne 0 -or $owner.Sid -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value) { throw 'upgrade_target_owner_mismatch' }
    return [pscustomobject]@{pid=$ProcessId; path=[IO.Path]::GetFullPath($process.ExecutablePath); started=$process.CreationDate.ToUniversalTime().ToString('o'); session_id=$process.SessionId; command_line=$process.CommandLine}
}

function Assert-UpgradeIdentity {
    param([Parameter(Mandatory)]$Expected)
    $actual = Get-UpgradeIdentity $Expected.pid
    # PowerShell 7 can parse ISO JSON dates into DateTime automatically; compare
    # instants at full precision, not culture-dependent string conversions.
    $expectedStarted = if ($Expected.started -is [DateTime]) { $Expected.started.ToUniversalTime().Ticks } else { [DateTimeOffset]::Parse($Expected.started).UtcTicks }
    $actualStarted = [DateTimeOffset]::Parse($actual.started).UtcTicks
    if ($actual.path -ne $Expected.path -or $actualStarted -ne $expectedStarted -or $actual.session_id -ne $Expected.session_id -or $actual.command_line -ne $Expected.command_line) { throw 'upgrade_target_identity_changed' }
}

function Get-UpgradeRuntimeFamily {
    param($Identity)
    $children = @()
    foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='redclaw_desktop.exe'" | Where-Object {$_.ExecutablePath -eq $Identity.path -and $_.ProcessId -ne $Identity.pid})) {
        if ($process.ParentProcessId -ne $Identity.pid -or $process.SessionId -ne $Identity.session_id) { throw 'upgrade_ambiguous_runtime_identity' }
        $children += Get-UpgradeIdentity $process.ProcessId
    }
    return $children
}

function Assert-UpgradeRuntimeFamily {
    param($Identity, [object[]]$Expected)
    $actual = @(Get-UpgradeRuntimeFamily $Identity)
    if ($actual.Count -ne $Expected.Count) { throw 'upgrade_runtime_identity_changed' }
    foreach ($entry in $Expected) { Assert-UpgradeIdentity $entry }
}

function ConvertFrom-UpgradeCommandLine {
    param([string]$CommandLine)
    if (-not ('RedClawUpgradeArguments' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RedClawUpgradeArguments {
    [DllImport("shell32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CommandLineToArgvW(string cmd, out int count);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr pointer);
    public static string[] Parse(string command) {
        int count; IntPtr memory = CommandLineToArgvW(command, out count);
        if (memory == IntPtr.Zero) throw new InvalidOperationException("argv_parse_failed");
        try { var args = new string[count]; for (int i=0; i<count; ++i) args[i]=Marshal.PtrToStringUni(Marshal.ReadIntPtr(memory, i*IntPtr.Size)); return args; }
        finally { LocalFree(memory); }
    }
}
'@
    }
    return [RedClawUpgradeArguments]::Parse($CommandLine)
}

function ConvertTo-UpgradeArgument {
    param([AllowEmptyString()][string]$Value)
    return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Write-UpgradeReceipt {
    param([string]$Path, [string]$Phase, [int]$TargetPid, [string]$Detail = '')
    [ordered]@{schema='redclaw.runtime-upgrade.status.v1'; worker_pid=$PID; phase=$Phase; target_pid=$TargetPid; updated_at=[DateTimeOffset]::UtcNow.ToString('o'); detail=$Detail} |
        ConvertTo-Json | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Start-UpgradedGui {
    param([string]$Executable, [string[]]$Argument, [string]$WorkingDirectory)
    $line = ($Argument | ForEach-Object {ConvertTo-UpgradeArgument $_}) -join ' '
    # This is the user-facing GUI whose restart was requested.
    return Start-Process -FilePath $Executable -ArgumentList $line -WorkingDirectory $WorkingDirectory -PassThru -WindowStyle Normal
}

function Stop-UpgradeGui {
    param($Identity, [string]$ControlName, [string]$Role)
    Assert-UpgradeIdentity $Identity
    if ($ControlName) {
        $response = & (Join-Path $PSScriptRoot 'invoke-cross-lan-debug-control.ps1') -Action status -Role $Role -ControlName $ControlName -Json | ConvertFrom-Json
        if (-not $response.ok) { throw 'upgrade_debug_status_unavailable' }
        # The pipe's GUI PID must agree before asking it to exit.
        if ([int]$response.status.app_pid -ne [int]$Identity.pid -or $response.status.role -ne $Role) { throw 'upgrade_control_identity_mismatch' }
        & (Join-Path $PSScriptRoot 'invoke-cross-lan-debug-control.ps1') -Action exit -Role $Role -ControlName $ControlName -Json | Out-Null
    } else {
        $process = Get-Process -Id $Identity.pid
        if (-not $process.CloseMainWindow()) { throw 'upgrade_graceful_close_unavailable' }
    }
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(20)
    do {
        $remaining = @(Get-CimInstance Win32_Process -Filter "Name='redclaw_desktop.exe'" | Where-Object {$_.ExecutablePath -eq $Identity.path})
        if (-not $remaining.Count) { return }
        Start-Sleep -Milliseconds 200
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw 'upgrade_runtime_files_still_owned'
}

function Test-UpgradedGui {
    param([Diagnostics.Process]$Process)
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(15)
    do {
        $Process.Refresh()
        if ($Process.HasExited) { return $false }
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero -and ([DateTime]::Now - $Process.StartTime).TotalSeconds -ge 5) { return $true }
        Start-Sleep -Milliseconds 200
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    return $false
}
