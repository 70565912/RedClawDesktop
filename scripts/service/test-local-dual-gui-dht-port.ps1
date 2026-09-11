$ErrorActionPreference = 'Stop'

# Load only the production port helpers, without starting GUIs or signaling.
$source = Join-Path $PSScriptRoot 'run-local-dual-gui-integration-test.ps1'
$parseTokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $source, [ref]$parseTokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Integration script has parse errors.' }
foreach ($name in @('Test-AvailableDhtPort', 'Get-AvailableDhtPort')) {
    $definition = $ast.Find({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false)
    if ($null -eq $definition) { throw "Missing production helper: $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}

# Rebinding proves all four production-probe sockets were disposed.
foreach ($iteration in 1..16) {
    $port = Get-AvailableDhtPort
    if ($port -lt 49152 -or $port -gt 65535 -or -not (Test-AvailableDhtPort -Port $port)) {
        throw "Production probe/rebind failed at iteration $iteration."
    }
}

# Deterministic refusal/exhaustion uses the production allocator, not a copy.
$script:probeCalls = 0
function Test-AvailableDhtPort {
    param([int]$Port)
    if ($Port -lt 49152 -or $Port -gt 65535) { throw 'Invalid candidate range.' }
    ++$script:probeCalls
    return $script:probeCalls -eq 64
}
$null = Get-AvailableDhtPort
if ($script:probeCalls -ne 64) { throw 'Allocator did not retry bounded refusal.' }
$script:probeCalls = 64
$boundedFailure = $false
try { $null = Get-AvailableDhtPort } catch {
    $boundedFailure = $_.Exception.Message -like 'Unable to reserve a DHT port*'
}
if (-not $boundedFailure -or $script:probeCalls -ne 128) {
    throw 'Allocator exhaustion must stop after exactly 64 attempts.'
}
Write-Output 'Passed: 16 four-bind/rebind probes, refusal retry, 64-attempt exhaustion.'
