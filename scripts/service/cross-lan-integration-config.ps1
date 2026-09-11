# Shared cross-LAN GUI integration constants.
# Keep in sync with docs/testing/cross-lan-dual-machine-integration-playbook.md

$script:CrossLanIntegrationSessionCode = 'RC7TST01'
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
