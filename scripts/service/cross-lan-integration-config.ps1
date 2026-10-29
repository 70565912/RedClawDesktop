# Shared cross-LAN GUI integration network defaults; private codes stay local.
# Keep in sync with docs/testing/cross-lan-dual-machine-integration-playbook.md

function Resolve-CrossLanIntegrationSessionCode {
    param(
        [string]$SessionCode = '',
        [string]$LocalConfigPath = (Join-Path $PSScriptRoot '..\..\local\cross-lan-session-code.txt')
    )

    if ([string]::IsNullOrWhiteSpace($SessionCode)) {
        if (-not (Test-Path -LiteralPath $LocalConfigPath -PathType Leaf)) {
            throw 'Provide -SessionCode or privately configure local/cross-lan-session-code.txt. No shared debug code is supplied by the repository.'
        }
        $SessionCode = ([string](Get-Content -LiteralPath $LocalConfigPath -Raw)).Trim()
    }
    if ($SessionCode -cnotmatch '^[A-Za-z0-9]{8}$') {
        throw 'The private SessionCode must contain exactly 8 letters or digits.'
    }
    return $SessionCode
}

# 0 = wait indefinitely for the peer; do not use a short timeout for cross-LAN runs.
$script:CrossLanIntegrationSignalTimeoutSeconds = 0
$script:CrossLanIntegrationIceServers = @(
    'stun:stun.douyucdn.cn:18000',
    'stun:stun.l.google.com:19302',
    'stun:stun.cloudflare.com:3478'
)
$script:CrossLanIntegrationDhtBootstrap = @(
    'router.bittorrent.com:6881',
    'dht.transmissionbt.com:6881',
    'dht.libtorrent.org:25401',
    'router.utorrent.com:6881'
)
