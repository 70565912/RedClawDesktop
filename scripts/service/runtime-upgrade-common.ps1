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
    if ($expected.ContainsKey('terminal-runtime.json')) {
        foreach ($required in @('terminal-runtime\msedgewebview2.exe','terminal-runtime\msedge.dll',
            'terminal-runtime\icudtl.dat','terminal-runtime\resources.pak',
            'maintenance\terminal-profile.ps1','maintenance\start-runtime-maintenance.ps1',
            'maintenance\runtime-upgrade-common.ps1','maintenance\invoke-runtime-directory-upgrade.ps1')) {
            if (-not $expected.ContainsKey($required)) { throw ('upgrade_bundle_missing_' + $required) }
        }
    }
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
    [ordered]@{schema='redclaw.runtime-upgrade.status.v1'; operation_id=(Split-Path (Split-Path $Path) -Leaf); worker_pid=$PID; phase=$Phase; target_pid=$TargetPid; updated_at=[DateTimeOffset]::UtcNow.ToString('o'); detail=$Detail} |
        ConvertTo-Json | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Get-UpgradeReceipt {
    param([string]$Path)
    $receipt = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($receipt.schema -ne 'redclaw.runtime-upgrade.status.v1') { throw 'upgrade_receipt_schema_invalid' }
    if ($receipt.phase -in @('preparing','handoff_ready','stopping','starting')) {
        $worker = Get-Process -Id ([int]$receipt.worker_pid) -ErrorAction SilentlyContinue
        $updated = if ($receipt.updated_at -is [DateTime]) { [DateTimeOffset]$receipt.updated_at } else { [DateTimeOffset]::Parse($receipt.updated_at) }
        if (-not $worker -or $worker.StartTime.ToUniversalTime() -gt $updated.UtcDateTime) {
            # Report an incomplete operation without rewriting its original
            # evidence or replaying it. A reused worker PID is not ownership.
            $receipt | Add-Member -NotePropertyName interrupted_phase -NotePropertyValue $receipt.phase
            $receipt.phase = 'interrupted'
        }
    }
    return $receipt
}

function Wait-UpgradeHandoff {
    param($Identity, [string]$StatusPath, [string]$AcknowledgmentPath, [string]$PlanHash,
        [string]$WorkerExecutable, [string[]]$WorkerArguments)
    # Bound each forward preflight stage, not the sum of full-bundle hashes and
    # copies. Repeated/unknown stages cannot extend the wait indefinitely.
    $stages = @('validating_candidate','validating_original','copying_candidate','validating_pending','probing_candidate')
    $lastStage = -1
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(120)
    do {
        if (Test-Path -LiteralPath $StatusPath) {
            try { $status = Get-Content -LiteralPath $StatusPath -Raw | ConvertFrom-Json } catch { $status = $null }
            if ($status -and $status.phase -eq 'preparing') {
                $stage = [Array]::IndexOf($stages, [string]$status.detail)
                if ($stage -gt $lastStage) {
                    $lastStage = $stage
                    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(120)
                }
            }
            if ($status -and $status.phase -eq 'handoff_ready') {
                Assert-UpgradeIdentity $Identity
                $workerIdentity = Get-UpgradeIdentity ([int]$status.worker_pid)
                $process = Get-CimInstance Win32_Process -Filter "ProcessId=$($status.worker_pid)"
                if ($workerIdentity.session_id -ne $Identity.session_id -or $process.ParentProcessId -eq $PID -or
                    $workerIdentity.path -ne $WorkerExecutable) { throw 'upgrade_worker_identity_mismatch' }
                $actualArguments = @(ConvertFrom-UpgradeCommandLine $workerIdentity.command_line | Select-Object -Skip 1)
                if ($actualArguments.Count -ne $WorkerArguments.Count) { throw 'upgrade_worker_arguments_mismatch' }
                for ($index = 0; $index -lt $WorkerArguments.Count; ++$index) {
                    if ($actualArguments[$index] -cne $WorkerArguments[$index]) { throw 'upgrade_worker_arguments_mismatch' }
                }
                $PlanHash | Set-Content -LiteralPath $AcknowledgmentPath -Encoding ASCII
                return [int]$status.worker_pid
            }
            if ($status -and $status.phase -eq 'failed') { throw ('upgrade_worker_preflight_failed: ' + $status.detail) }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw 'upgrade_worker_handoff_timeout_target_not_stopped'
}

function Start-UpgradedGui {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseShouldProcessForStateChangingFunctions','',Justification='Internal fixed step under the validated upgrade handoff.') ]
    [CmdletBinding()]
    param([string]$Executable, [string[]]$Argument, [string]$WorkingDirectory)
    $line = ($Argument | ForEach-Object {ConvertTo-UpgradeArgument $_}) -join ' '
    # This is the user-facing GUI whose restart was requested.
    return Start-Process -FilePath $Executable -ArgumentList $line -WorkingDirectory $WorkingDirectory -PassThru -WindowStyle Normal
}

function Stop-UpgradeGui {
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseShouldProcessForStateChangingFunctions','',Justification='Internal fixed step under the validated upgrade handoff.') ]
    [CmdletBinding()]
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
    param([Diagnostics.Process]$Process, [switch]$RequireHostRuntime)
    # Measured cold Host initialization can precede runtime creation by 26s.
    # This process-start deadline is separate from GUI/media heartbeat gates.
    $startupSeconds = if ($RequireHostRuntime) { 60 } else { 15 }
    $deadline = [DateTimeOffset]$Process.StartTime.ToUniversalTime().AddSeconds($startupSeconds)
    do {
        $Process.Refresh()
        if ($Process.HasExited) { return $false }
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero -and ([DateTime]::Now - $Process.StartTime).TotalSeconds -ge 5) {
            if (-not $RequireHostRuntime) { return $true }
            $family = @(Get-UpgradeRuntimeFamily (Get-UpgradeIdentity $Process.Id))
            if ($family.Count -eq 1 -and ([DateTimeOffset]::UtcNow - [DateTimeOffset]::Parse($family[0].started)).TotalSeconds -ge 2) { return $true }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    return $false
}

function Wait-UpgradeBundleReleased {
    param([string]$Directory, [object[]]$Manifest)
    # WebView2 can finish closing after its owning GUI exits. Wait for every
    # bundle file to be free; never kill a browser to force a directory swap.
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(20)
    do {
        try {
            foreach ($entry in $Manifest) {
                $handle = [IO.File]::Open((Join-Path $Directory $entry.path),'Open','Read','None')
                $handle.Dispose()
            }
            return
        } catch {
            if ($_.Exception.GetBaseException() -isnot [IO.IOException] -or [DateTimeOffset]::UtcNow -ge $deadline) { throw }
        }
        Start-Sleep -Milliseconds 200
    } while ($true)
}
