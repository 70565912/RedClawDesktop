param(
    [Parameter(Mandatory = $true)][string]$CodecPath
)
$ErrorActionPreference = 'Stop'
$TimeoutMs = 5000
$script:MaxFrameBytes = 64 * 1024
$tokens = $null
$parseErrors = $null
$client = Join-Path $PSScriptRoot 'invoke-agent-control.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($client, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw 'Agent client script does not parse.' }
# Exercise the actual client's codec path without touching identity files, ACLs,
# running providers or opening its Named Pipe. Only these three pure functions load.
foreach ($name in @('Invoke-AgentWireCodec', 'New-AgentFrame', 'Read-AgentFrame')) {
    $definition = $ast.Find({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if ($null -eq $definition) { throw 'Production codec function missing.' }
    . ([scriptblock]::Create($definition.Extent.Text))
}
function Assert-Codec {
    param([bool]$Condition, [string]$Reason)
    if (-not $Condition) { throw $Reason }
}
$identity = @{ SessionEpoch = 'codec-fixture'; MessageId = 1 }
$fields = @{
    message_type = 'agent_task_create'; provider = 'codex'; task_id = 'codec-task'
    project_id = 'opaque-project'; request_id = 'codec-request'; text = "line=one`nline\two"
}
$frame = New-AgentFrame -Fields $fields -Identity $identity
$decoded = Read-AgentFrame -Frame $frame
Assert-Codec ($decoded.text -eq $fields.text) 'Codec lost escaping/newline content.'
Assert-Codec ($decoded.message_type -eq $fields.message_type) 'Codec changed message type.'
Assert-Codec ($decoded.message_id -eq '1') 'Codec changed identity.'
Assert-Codec ($frame.StartsWith('RCD-LOCAL-AGENT-V1 ') -and -not $frame.Contains("`n")) 'Not one bounded Base64 frame.'
$fields.text = ([string][char]0x4E2D) * 5000
$unicode = New-AgentFrame -Fields $fields -Identity $identity
Assert-Codec ((Read-AgentFrame -Frame $unicode).text -eq $fields.text) 'Codec lost UTF-8 text.'
$rejected = 0
foreach ($mutation in @('oversized', 'bad_provider', 'bad_type', 'trailing_bytes')) {
    $bad = $fields.Clone()
    try {
        switch ($mutation) {
            'oversized' { $bad.text = 'x' * 16385 }
            'bad_provider' { $bad.provider = 'unknown-provider' }
            'bad_type' { $bad.message_type = 'execute_arbitrary_shell' }
            'trailing_bytes' { [void](Read-AgentFrame -Frame ($frame + 'garbage')); continue }
        }
        [void](New-AgentFrame -Fields $bad -Identity $identity)
    } catch { $rejected++ }
}
Assert-Codec ($rejected -eq 4) 'Invalid codec input was accepted.'
[pscustomobject]@{ passed = $true; rejected_cases = $rejected; frame_bytes = $frame.Length; unicode_frame_bytes = $unicode.Length } |
    ConvertTo-Json -Compress
