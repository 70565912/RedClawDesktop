param(
    [string]$UrlPrefix = "http://127.0.0.1:8467/api/"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:Sessions = @{}
$script:Signals = @{}

function Get-UnixTimeSeconds {
    return [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
}

function Escape-TransportField {
    param(
        [AllowEmptyString()]
        [string]$Value
    )

    $escaped = $Value.Replace("\", "\\")
    $escaped = $escaped.Replace("`t", "\t")
    $escaped = $escaped.Replace("`n", "\n")
    $escaped = $escaped.Replace("`r", "\r")
    return $escaped
}

function Unescape-TransportField {
    param(
        [AllowEmptyString()]
        [string]$Value
    )

    $builder = New-Object System.Text.StringBuilder
    $escapePending = $false
    foreach ($character in $Value.ToCharArray()) {
        if (-not $escapePending) {
            if ($character -eq '\') {
                $escapePending = $true
                continue
            }

            [void]$builder.Append($character)
            continue
        }

        switch ($character) {
            'n' { [void]$builder.Append("`n") }
            'r' { [void]$builder.Append("`r") }
            't' { [void]$builder.Append("`t") }
            '\' { [void]$builder.Append('\') }
            default { throw "Invalid escaped transport field sequence '\$character'." }
        }

        $escapePending = $false
    }

    if ($escapePending) {
        throw "Transport field ended with a dangling escape sequence."
    }

    return $builder.ToString()
}

function Parse-TransportLine {
    param(
        [string]$Line
    )

    $fields = New-Object System.Collections.Generic.List[string]
    $current = New-Object System.Text.StringBuilder
    $escapePending = $false

    foreach ($character in $Line.ToCharArray()) {
        if (-not $escapePending) {
            if ($character -eq '\') {
                $escapePending = $true
                continue
            }

            if ($character -eq "`t") {
                $fields.Add((Unescape-TransportField -Value $current.ToString()))
                [void]$current.Clear()
                continue
            }

            [void]$current.Append($character)
            continue
        }

        [void]$current.Append('\')
        [void]$current.Append($character)
        $escapePending = $false
    }

    if ($escapePending) {
        throw "Transport line ended with a dangling escape sequence."
    }

    $fields.Add((Unescape-TransportField -Value $current.ToString()))
    return ,$fields.ToArray()
}

function Parse-TransportDocument {
    param(
        [AllowEmptyString()]
        [string]$Body
    )

    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($line in ($Body -split "`n")) {
        $normalizedLine = $line.TrimEnd("`r")
        if ([string]::IsNullOrEmpty($normalizedLine)) {
            continue
        }

        $rows.Add((Parse-TransportLine -Line $normalizedLine))
    }

    return ,$rows.ToArray()
}

function Format-TransportLine {
    param(
        [string[]]$Fields
    )

    $escapedFields = foreach ($field in $Fields) {
        Escape-TransportField -Value $field
    }

    return (($escapedFields -join "`t") + "`n")
}

function Test-SessionCode {
    param(
        [string]$Value
    )

    return $Value -match '^[A-Z0-9]{8}$'
}

function Remove-SessionState {
    param(
        [string]$SessionCode,
        [hashtable]$Entry
    )

    [void]$script:Sessions.Remove($SessionCode)
    [void]$script:Signals.Remove("$($Entry.SessionId)|host")
    [void]$script:Signals.Remove("$($Entry.SessionId)|controller")
}

function Cleanup-ExpiredSessions {
    $now = Get-UnixTimeSeconds
    foreach ($sessionCode in @($script:Sessions.Keys)) {
        $entry = $script:Sessions[$sessionCode]
        if ($entry.ExpiresAtUnix -le $now) {
            Remove-SessionState -SessionCode $sessionCode -Entry $entry
            Write-Host "[rendezvous] expired session removed session_code=$sessionCode session_id=$($entry.SessionId)"
        }
    }
}

function Find-SessionById {
    param(
        [string]$SessionId
    )

    foreach ($entry in $script:Sessions.Values) {
        if ($entry.SessionId -eq $SessionId) {
            return $entry
        }
    }

    return $null
}

function Read-RequestBody {
    param(
        [System.Net.HttpListenerRequest]$Request
    )

    $reader = [System.IO.StreamReader]::new($Request.InputStream, $Request.ContentEncoding)
    try {
        return $reader.ReadToEnd()
    }
    finally {
        $reader.Dispose()
    }
}

function Write-PlainTextResponse {
    param(
        [System.Net.HttpListenerResponse]$Response,
        [int]$StatusCode,
        [AllowEmptyString()]
        [string]$Body
    )

    $payload = [System.Text.Encoding]::UTF8.GetBytes($Body)
    $Response.StatusCode = $StatusCode
    $Response.ContentType = "text/plain; charset=utf-8"
    $Response.ContentEncoding = [System.Text.Encoding]::UTF8
    $Response.ContentLength64 = $payload.LongLength
    $Response.OutputStream.Write($payload, 0, $payload.Length)
    $Response.OutputStream.Close()
}

function Get-RelativeRequestPath {
    param(
        [System.Net.HttpListenerRequest]$Request,
        [string]$BasePath
    )

    $path = $Request.Url.AbsolutePath
    if ([string]::IsNullOrEmpty($BasePath) -or $BasePath -eq "/") {
        return $path
    }

    if (-not $path.StartsWith($BasePath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }

    $relative = $path.Substring($BasePath.Length)
    if ([string]::IsNullOrEmpty($relative)) {
        return "/"
    }

    if (-not $relative.StartsWith("/")) {
        return "/$relative"
    }

    return $relative
}

function New-SessionRegisterResponse {
    param(
        [bool]$Accepted,
        [string]$Error,
        [Int64]$ExpiresAtUnix
    )

    return (Format-TransportLine -Fields @(
            "result",
            $(if ($Accepted) { "1" } else { "0" }),
            $Error,
            [string]$ExpiresAtUnix
        ))
}

function New-SessionLookupResponse {
    param(
        [bool]$Found,
        [bool]$Claimed,
        [string]$Error,
        [string]$SessionId,
        [string]$HostDisplayName,
        [string]$HostFingerprintSummary,
        [Int64]$ExpiresAtUnix
    )

    return (Format-TransportLine -Fields @(
            "lookup",
            $(if ($Found) { "1" } else { "0" }),
            $(if ($Claimed) { "1" } else { "0" }),
            $Error,
            $SessionId,
            $HostDisplayName,
            $HostFingerprintSummary,
            [string]$ExpiresAtUnix
        ))
}

function New-SessionClaimResponse {
    param(
        [bool]$Claimed,
        [string]$Error,
        [string]$SessionId
    )

    return (Format-TransportLine -Fields @(
            "claim",
            $(if ($Claimed) { "1" } else { "0" }),
            $Error,
            $SessionId
        ))
}

function New-SignalPublishResponse {
    param(
        [bool]$Accepted,
        [string]$Error,
        [Int64]$Revision,
        [Int64]$UpdatedAtUnix
    )

    return (Format-TransportLine -Fields @(
            "publish",
            $(if ($Accepted) { "1" } else { "0" }),
            $Error,
            [string]$Revision,
            [string]$UpdatedAtUnix
        ))
}

function New-SignalFetchResponse {
    param(
        [hashtable]$SignalEntry
    )

    $body = Format-TransportLine -Fields @(
        "fetch",
        "1",
        "none",
        $SignalEntry.DescriptionType,
        $SignalEntry.DescriptionSdp,
        [string]$SignalEntry.Revision,
        [string]$SignalEntry.UpdatedAtUnix
    )

    foreach ($candidateLine in $SignalEntry.CandidateLines) {
        $parts = $candidateLine -split "`t", 2
        if ($parts.Count -lt 2) {
            continue
        }

        $body += Format-TransportLine -Fields @(
            "candidate",
            $parts[0],
            $parts[1]
        )
    }

    return $body
}

$prefixUri = [Uri]$UrlPrefix
$basePath = $prefixUri.AbsolutePath.TrimEnd('/')
if ([string]::IsNullOrEmpty($basePath)) {
    $basePath = "/"
}

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add($UrlPrefix)
$listener.Start()

Write-Host "[rendezvous] listening prefix=$UrlPrefix"
Write-Host "[rendezvous] expected runtime base URL=$($prefixUri.Scheme)://$($prefixUri.Authority)$basePath"

try {
    while ($listener.IsListening) {
        $context = $listener.GetContext()
        try {
            Cleanup-ExpiredSessions

            $request = $context.Request
            $response = $context.Response
            $path = Get-RelativeRequestPath -Request $request -BasePath $basePath
            if ($null -eq $path) {
                Write-PlainTextResponse -Response $response -StatusCode 404 -Body ""
                continue
            }

            if ($request.HttpMethod -eq "PUT" -and $path -match '^/v1/sessions/([^/]+)$') {
                $sessionCode = [Uri]::UnescapeDataString($matches[1]).ToUpperInvariant()
                if (-not (Test-SessionCode -Value $sessionCode)) {
                    Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SessionRegisterResponse -Accepted $false -Error "invalid_request" -ExpiresAtUnix 0)
                    continue
                }

                $rows = Parse-TransportDocument -Body (Read-RequestBody -Request $request)
                if ($rows.Count -eq 0 -or $rows[0].Count -lt 5 -or $rows[0][0] -ne "register") {
                    Write-PlainTextResponse -Response $response -StatusCode 400 -Body "invalid register body`n"
                    continue
                }

                [uint32]$ttlSeconds = 0
                if (-not [uint32]::TryParse($rows[0][4], [ref]$ttlSeconds)) {
                    Write-PlainTextResponse -Response $response -StatusCode 400 -Body "invalid ttl`n"
                    continue
                }

                if ($script:Sessions.ContainsKey($sessionCode)) {
                    $existing = $script:Sessions[$sessionCode]
                    Write-Host "[rendezvous] register rejected duplicate session_code=$sessionCode session_id=$($existing.SessionId)"
                    Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SessionRegisterResponse -Accepted $false -Error "code_already_exists" -ExpiresAtUnix $existing.ExpiresAtUnix)
                    continue
                }

                $expiresAtUnix = (Get-UnixTimeSeconds) + [int64]$ttlSeconds
                $script:Sessions[$sessionCode] = @{
                    SessionCode = $sessionCode
                    SessionId = $rows[0][1]
                    HostDisplayName = $rows[0][2]
                    HostFingerprintSummary = $rows[0][3]
                    ExpiresAtUnix = $expiresAtUnix
                    Claimed = $false
                }

                Write-Host "[rendezvous] registered session_code=$sessionCode session_id=$($rows[0][1]) expires_at_unix=$expiresAtUnix"
                Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SessionRegisterResponse -Accepted $true -Error "none" -ExpiresAtUnix $expiresAtUnix)
                continue
            }

            if ($request.HttpMethod -eq "GET" -and $path -match '^/v1/sessions/([^/]+)$') {
                $sessionCode = [Uri]::UnescapeDataString($matches[1]).ToUpperInvariant()
                if (-not (Test-SessionCode -Value $sessionCode)) {
                    Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SessionLookupResponse -Found $false -Claimed $false -Error "invalid_request" -SessionId "" -HostDisplayName "" -HostFingerprintSummary "" -ExpiresAtUnix 0)
                    continue
                }

                if (-not $script:Sessions.ContainsKey($sessionCode)) {
                    Write-PlainTextResponse -Response $response -StatusCode 404 -Body ""
                    continue
                }

                $entry = $script:Sessions[$sessionCode]
                Write-Host "[rendezvous] lookup hit session_code=$sessionCode claimed=$($entry.Claimed.ToString().ToLowerInvariant()) session_id=$($entry.SessionId)"
                Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SessionLookupResponse -Found $true -Claimed $entry.Claimed -Error "none" -SessionId $entry.SessionId -HostDisplayName $entry.HostDisplayName -HostFingerprintSummary $entry.HostFingerprintSummary -ExpiresAtUnix $entry.ExpiresAtUnix)
                continue
            }

            if ($request.HttpMethod -eq "POST" -and $path -match '^/v1/sessions/([^/]+)/claim$') {
                $sessionCode = [Uri]::UnescapeDataString($matches[1]).ToUpperInvariant()
                if (-not (Test-SessionCode -Value $sessionCode)) {
                    Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SessionClaimResponse -Claimed $false -Error "invalid_request" -SessionId "")
                    continue
                }

                if (-not $script:Sessions.ContainsKey($sessionCode)) {
                    Write-PlainTextResponse -Response $response -StatusCode 404 -Body ""
                    continue
                }

                $entry = $script:Sessions[$sessionCode]
                if ($entry.Claimed) {
                    Write-Host "[rendezvous] claim rejected already_claimed session_code=$sessionCode session_id=$($entry.SessionId)"
                    Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SessionClaimResponse -Claimed $false -Error "already_claimed" -SessionId $entry.SessionId)
                    continue
                }

                $entry.Claimed = $true
                Write-Host "[rendezvous] claim succeeded session_code=$sessionCode session_id=$($entry.SessionId)"
                Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SessionClaimResponse -Claimed $true -Error "none" -SessionId $entry.SessionId)
                continue
            }

            if ($request.HttpMethod -eq "PUT" -and $path -match '^/v1/runtime-sessions/([^/]+)/(host|controller)-signal$') {
                $sessionId = [Uri]::UnescapeDataString($matches[1])
                $lane = $matches[2]
                $sessionEntry = Find-SessionById -SessionId $sessionId
                if ($null -eq $sessionEntry) {
                    Write-PlainTextResponse -Response $response -StatusCode 404 -Body ""
                    continue
                }

                $rows = Parse-TransportDocument -Body (Read-RequestBody -Request $request)
                if ($rows.Count -eq 0 -or $rows[0].Count -lt 3 -or $rows[0][0] -ne "signal") {
                    Write-PlainTextResponse -Response $response -StatusCode 400 -Body "invalid signal body`n"
                    continue
                }

                $candidateLines = New-Object System.Collections.Generic.List[string]
                for ($i = 1; $i -lt $rows.Count; ++$i) {
                    $row = $rows[$i]
                    if ($row.Count -lt 3 -or $row[0] -ne "candidate") {
                        continue
                    }

                    $candidateLines.Add("$($row[1])`t$($row[2])")
                }

                $signalKey = "$sessionId|$lane"
                $existingSignal = $script:Signals[$signalKey]
                $revision = 1
                if ($null -ne $existingSignal) {
                    $revision = [int64]$existingSignal.Revision + 1
                }

                $updatedAtUnix = Get-UnixTimeSeconds
                $script:Signals[$signalKey] = @{
                    DescriptionType = $rows[0][1]
                    DescriptionSdp = $rows[0][2]
                    CandidateLines = $candidateLines.ToArray()
                    Revision = $revision
                    UpdatedAtUnix = $updatedAtUnix
                }

                Write-Host "[rendezvous] signal published session_id=$sessionId lane=$lane revision=$revision candidates=$($candidateLines.Count)"
                Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SignalPublishResponse -Accepted $true -Error "none" -Revision $revision -UpdatedAtUnix $updatedAtUnix)
                continue
            }

            if ($request.HttpMethod -eq "GET" -and $path -match '^/v1/runtime-sessions/([^/]+)/(host|controller)-signal$') {
                $sessionId = [Uri]::UnescapeDataString($matches[1])
                $lane = $matches[2]
                $sessionEntry = Find-SessionById -SessionId $sessionId
                if ($null -eq $sessionEntry) {
                    Write-PlainTextResponse -Response $response -StatusCode 404 -Body ""
                    continue
                }

                $signalKey = "$sessionId|$lane"
                if (-not $script:Signals.ContainsKey($signalKey)) {
                    Write-PlainTextResponse -Response $response -StatusCode 404 -Body ""
                    continue
                }

                $signalEntry = $script:Signals[$signalKey]
                Write-Host "[rendezvous] signal fetched session_id=$sessionId lane=$lane revision=$($signalEntry.Revision)"
                Write-PlainTextResponse -Response $response -StatusCode 200 -Body (New-SignalFetchResponse -SignalEntry $signalEntry)
                continue
            }

            Write-PlainTextResponse -Response $response -StatusCode 404 -Body ""
        }
        catch {
            Write-Host "[rendezvous] request handling failed: $($_.Exception.Message)"
            Write-PlainTextResponse -Response $context.Response -StatusCode 500 -Body ("server error: " + $_.Exception.Message + "`n")
        }
    }
}
finally {
    if ($listener.IsListening) {
        $listener.Stop()
    }
    $listener.Close()
}