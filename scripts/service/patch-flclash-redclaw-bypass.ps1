# Patch FlClash profile + override rules to bypass redclaw_desktop and DHT bootstrap traffic.
# Re-run after subscription refresh if the profile yaml is overwritten.

param(
    [string]$FlClashConfigRoot = (Join-Path $env:APPDATA "com.follow\clash"),
    [switch]$WhatIf
)

$ErrorActionPreference = "Stop"

function Get-ActiveProfilePath {
    param([string]$Root)

    $prefsPath = Join-Path $Root "shared_preferences.json"
    if (-not (Test-Path $prefsPath)) {
        throw "FlClash preferences not found: $prefsPath"
    }

    $prefsRaw = Get-Content -Path $prefsPath -Raw -Encoding UTF8
    $prefs = $prefsRaw | ConvertFrom-Json
    $configJson = $prefs.'flutter.config' | ConvertFrom-Json
    $profileId = [string]$configJson.currentProfileId
    if ([string]::IsNullOrWhiteSpace($profileId)) {
        throw "currentProfileId missing in FlClash preferences"
    }

    $profilePath = Join-Path $Root ("profiles\{0}.yaml" -f $profileId)
    if (-not (Test-Path $profilePath)) {
        throw "Active profile not found: $profilePath"
    }

    return [pscustomobject]@{
        PreferencesPath = $prefsPath
        PreferencesRaw = $prefsRaw
        Preferences = $prefs
        ConfigJson = $configJson
        ProfilePath = $profilePath
        ProfileId = $profileId
    }
}

$redclawRules = @(
    "PROCESS-NAME,redclaw_desktop.exe,DIRECT",
    "PROCESS-NAME,redclaw_host_service.exe,DIRECT",
    "DOMAIN,router.bittorrent.com,DIRECT",
    "DOMAIN,dht.transmissionbt.com,DIRECT",
    "DOMAIN,dht.libtorrent.org,DIRECT",
    "DOMAIN,router.utorrent.com,DIRECT",
    "IP-CIDR,67.215.246.10/32,DIRECT,no-resolve",
    "IP-CIDR,87.98.162.88/32,DIRECT,no-resolve",
    "IP-CIDR,212.129.33.59/32,DIRECT,no-resolve",
    "IP-CIDR,185.157.221.247/32,DIRECT,no-resolve",
    "IP-CIDR,82.221.103.244/32,DIRECT,no-resolve"
)

$fakeIpFilterEntries = @(
    "+.router.bittorrent.com",
    "+.dht.transmissionbt.com",
    "+.dht.libtorrent.org",
    "+.router.utorrent.com"
)

$marker = "# RedClaw bypass"

function Test-RulePresent {
    param([string[]]$Rules, [string]$Needle)
    return ($Rules | Where-Object { $_ -eq $Needle }).Count -gt 0
}

function Patch-ProfileYaml {
    param([string]$Path)

    $content = Get-Content -Path $Path -Raw -Encoding UTF8
    $changed = $false

    $needsFakeIpFilter = $false
    foreach ($entry in $fakeIpFilterEntries) {
        if ($content -notmatch [regex]::Escape($entry)) {
            $needsFakeIpFilter = $true
            break
        }
    }

    if ($needsFakeIpFilter) {
        $suffix = ($fakeIpFilterEntries | ForEach-Object { "'$_'" }) -join ", "
        if ($content -match "\+\.media\.dssott\.com, \+\.pvp\.net\]") {
            $content = $content -replace "\+\.media\.dssott\.com, \+\.pvp\.net\]", "+.media.dssott.com, +.pvp.net, $suffix]"
            $changed = $true
        }
        elseif ($content -match "\+\.media\.dssott\.com, \+\.pvp\.net'\]") {
            $content = $content -replace "\+\.media\.dssott\.com, \+\.pvp\.net'\]", "+.media.dssott.com, +.pvp.net, $suffix]"
            $changed = $true
        }
        elseif ($content -match '\+\.pvp\.net"\]') {
            $content = $content -replace '\+\.pvp\.net"\]', ('+.pvp.net", {0}]' -f (($fakeIpFilterEntries | ForEach-Object { "`"$_`"" }) -join ", "))
            $changed = $true
        }
    }

    if ($content -notmatch [regex]::Escape($marker)) {
        $ruleBlock = ($redclawRules | ForEach-Object { "    - '$($_)'" }) -join "`n"
        $insert = "    - '$marker'`n$ruleBlock`n"
        if ($content -match "(?m)^rules:\r?\n") {
            $content = [regex]::Replace($content, "(?m)^rules:\r?\n", ($insert), 1)
            $changed = $true
        }
    }

    if (-not $changed) {
        Write-Host "[profile] already patched: $Path"
        return
    }

    if ($WhatIf) {
        Write-Host "[profile] would patch: $Path"
        return
    }

    Copy-Item -Path $Path -Destination ($Path + ".bak-redclaw") -Force
    Set-Content -Path $Path -Value $content -Encoding UTF8 -NoNewline
    Write-Host "[profile] patched: $Path"
}

function Patch-PreferencesOverride {
    param(
        [string]$Path,
        [object]$ConfigJson
    )

    if (-not $ConfigJson.PSObject.Properties.Name.Contains("patchClashConfig")) {
        $ConfigJson | Add-Member -NotePropertyName patchClashConfig -NotePropertyValue ([pscustomobject]@{})
    }

    $patch = $ConfigJson.patchClashConfig
    if (-not $patch.PSObject.Properties.Name.Contains("rule")) {
        $patch | Add-Member -NotePropertyName rule -NotePropertyValue @()
    }

    $existingRules = @()
    if ($null -ne $patch.rule) {
        $existingRules = @($patch.rule)
    }

    $mergedRules = @($existingRules)
    foreach ($rule in $redclawRules) {
        if (-not (Test-RulePresent -Rules $mergedRules -Needle $rule)) {
            $mergedRules = @($rule) + $mergedRules
        }
    }

    $changed = ($mergedRules.Count -ne $existingRules.Count)
    if (-not $changed) {
        Write-Host "[override] rules already present in patchClashConfig"
        return $false
    }

    if ($WhatIf) {
        Write-Host "[override] would prepend $($mergedRules.Count - $existingRules.Count) rule(s) to patchClashConfig"
        return $true
    }

    $patch.rule = $mergedRules
    $prefs = Get-Content -Path $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    $prefs.'flutter.config' = ($ConfigJson | ConvertTo-Json -Depth 64 -Compress)
    Copy-Item -Path $Path -Destination ($Path + ".bak-redclaw") -Force
    Set-Content -Path $Path -Value ($prefs | ConvertTo-Json -Depth 8 -Compress) -Encoding UTF8 -NoNewline
    Write-Host "[override] patched patchClashConfig.rule in: $Path"
    return $true
}

$active = Get-ActiveProfilePath -Root $FlClashConfigRoot
Write-Host "[flclash] active profile id=$($active.ProfileId)"
Write-Host "[flclash] profile path=$($active.ProfilePath)"

Patch-ProfileYaml -Path $active.ProfilePath
$overrideChanged = Patch-PreferencesOverride -Path $active.PreferencesPath -ConfigJson $active.ConfigJson

Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Open FlClash and reload the active profile (or restart FlClash)."
Write-Host "  2. Verify with: Resolve-DnsName router.bittorrent.com (should not be 28.0.0.x)."
Write-Host "  3. Re-run this script after subscription refresh if DHT domains revert."

if ($overrideChanged -or -not $WhatIf) {
    Write-Host "  Note: profile yaml edits may be overwritten by subscription refresh; patchClashConfig rules persist in preferences."
}
