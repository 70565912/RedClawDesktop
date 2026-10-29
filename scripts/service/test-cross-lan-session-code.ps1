$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cross-lan-integration-config.ps1')

$fixtureRoot = Join-Path $PSScriptRoot ('..\..\build\reports\debug-code-fixture-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
$fixtureFile = Join-Path $fixtureRoot 'session.txt'
$cases = 0
try {
    $null = Resolve-CrossLanIntegrationSessionCode -LocalConfigPath $fixtureFile
    throw 'Missing local configuration unexpectedly succeeded.'
} catch {
    if ($_.Exception.Message -notlike 'Provide -SessionCode*') { throw }
    ++$cases
}
if ((Resolve-CrossLanIntegrationSessionCode -SessionCode 'UNIT0001' -LocalConfigPath $fixtureFile) -ne 'UNIT0001') {
    throw 'Explicit private code was not preserved.'
}
++$cases
Set-Content -LiteralPath $fixtureFile -Value 'UNIT0002' -Encoding ASCII
if ((Resolve-CrossLanIntegrationSessionCode -LocalConfigPath $fixtureFile) -ne 'UNIT0002') {
    throw 'Local configuration was not resolved.'
}
++$cases
if ((Resolve-CrossLanIntegrationSessionCode -SessionCode 'UNIT0001' -LocalConfigPath $fixtureFile) -ne 'UNIT0001') {
    throw 'Explicit code must take precedence over local configuration.'
}
++$cases
foreach ($invalid in @('', 'invalid fixture value')) {
    Set-Content -LiteralPath $fixtureFile -Value $invalid -Encoding ASCII
    try {
        $null = Resolve-CrossLanIntegrationSessionCode -LocalConfigPath $fixtureFile
        throw 'Invalid local code unexpectedly succeeded.'
    } catch {
        if ($_.Exception.Message -ne 'The private SessionCode must contain exactly 8 letters or digits.') { throw }
        ++$cases
    }
}
Write-Output "Passed: $cases private-session-code cases; no GUI or networking started."
