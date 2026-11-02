$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-upgrade-common.ps1')
$baseline = [pscustomobject]@{
    app_pid=123; runtime_running=$true; role='host'; updated_at_unix_ms=5000
    dht=[pscustomobject]@{publish_success=1}
}
if (-not (Test-UpgradePublicationSnapshot -Snapshot $baseline -GuiProcessId 123 -StartedUnixMs 4000 -NowUnixMs 6000)) {
    throw 'fresh_matching_publication_rejected'
}
$cases = @(
    @{field='app_pid';value=124}, @{field='runtime_running';value=$false},
    @{field='role';value='controller'}, @{field='updated_at_unix_ms';value=3999},
    @{field='updated_at_unix_ms';value=7000},
    @{field='dht';value=[pscustomobject]@{publish_success=0}},
    @{field='dht';value=[pscustomobject]@{}}
)
foreach ($case in $cases) {
    $snapshot = $baseline | ConvertTo-Json | ConvertFrom-Json
    $snapshot.($case.field) = $case.value
    if (Test-UpgradePublicationSnapshot -Snapshot $snapshot -GuiProcessId 123 -StartedUnixMs 4000 -NowUnixMs 6000) {
        throw ('invalid_publication_accepted_' + $case.field)
    }
}
if (Test-UpgradePublicationSnapshot -Snapshot $baseline -GuiProcessId 123 -StartedUnixMs 4000 -NowUnixMs 21000) {
    throw 'stale_publication_accepted'
}
if (Test-UpgradePublicationSnapshot -Snapshot ([pscustomobject]@{}) -GuiProcessId 123 -StartedUnixMs 4000 -NowUnixMs 6000) {
    throw 'missing_publication_fields_accepted'
}
$fixtureDirectory=Join-Path ([IO.Path]::GetTempPath()) ('redclaw-publication-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureDirectory|Out-Null
try {
    $fixturePath=Join-Path $fixtureDirectory 'status.json'
    $process=Get-Process -Id $PID
    $note=([string][char]0x4E2D)+[char]0x6587+'"'+[char]0x5C3E
    $snapshot=[ordered]@{
        app_pid=$PID;runtime_running=$true;role='host'
        updated_at_unix_ms=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        dht=[ordered]@{publish_success=1}
        optional_note=$note
    }
    [IO.File]::WriteAllText($fixturePath,($snapshot|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
    if(-not (Test-UpgradedHostPublication -Process $process -StatusPath $fixturePath -TimeoutSeconds 1)) {
        throw 'utf8_without_bom_publication_rejected'
    }
    $snapshot.app_pid=$PID+1
    [IO.File]::WriteAllText($fixturePath,($snapshot|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
    if(Test-UpgradedHostPublication -Process $process -StatusPath $fixturePath -TimeoutSeconds 1) {
        throw 'different_process_publication_accepted'
    }
    [IO.File]::WriteAllText($fixturePath,'{"app_pid":',[Text.UTF8Encoding]::new($false))
    if(Test-UpgradedHostPublication -Process $process -StatusPath $fixturePath -TimeoutSeconds 1) {
        throw 'invalid_json_publication_accepted'
    }
} finally {
    # Only the generated file and its now-empty directory belong to this test.
    Remove-Item -LiteralPath (Join-Path $fixtureDirectory 'status.json') -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fixtureDirectory -ErrorAction SilentlyContinue
}
[pscustomobject]@{checks=13;passed=$true} | ConvertTo-Json
