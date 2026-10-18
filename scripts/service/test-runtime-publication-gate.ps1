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
[pscustomobject]@{checks=10;passed=$true} | ConvertTo-Json
