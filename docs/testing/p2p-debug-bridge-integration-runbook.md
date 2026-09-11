# P0-DS-10 P2P Debug Bridge Integration Runbook

For `P1-AG-02`, this bridge is a recovery authority, not a peer of the healthy
normal Agent API. First pause state-changing work and attempt bounded normal
Agent reconnect/task sync. Start Agent work on the bridge only after an
explicit owner-local journal transfer to `debug_bridge`; never poll both paths
for executable work. Recovery remains an explicit operator action.

## Preconditions

1. Record each endpoint's committed Git SHA, product/protocol version, negotiated capabilities, build configuration, and Debug bridge EXE SHA-256. Supported versions must interoperate and may use different commits or binaries; hash equality is required only when both endpoints run the same distributed artifact.
2. Each endpoint selects its physical Internet-facing adapter; avoid Auto,
   proxy/TUN/VPN, Hyper-V, and APIPA for the baseline.
3. Operators agree on one bridge ID, one fresh epoch, and a passphrase of at
   least 16 characters. Store it in a current-user-readable file outside Git.
4. Host runs the normal Debug RedClaw GUI with its typed Debug Control pipe.
   Agent relay additionally requires a registered-project manifest and explicit
   Host `-AllowAgentTasks` authorization.

Before a two-machine attempt, run the deterministic local gate. It creates and
removes its own random passphrase and bridge processes, and writes only bounded
evidence under `build/reports`:

```powershell
.\scripts\service\test-debug-bridge-local.ps1 `
  -TimeoutSeconds 45 `
  -RequireRedClawRelay
```

## Start and sealed-file exchange

Host starts first:

```powershell
.\scripts\service\start-debug-bridge.ps1 `
  -Role host `
  -SignalDirectory D:\RedClawDebugBridge\signal `
  -BridgeId RCD-DBG-01 `
  -SessionEpoch epoch-20260901-01 `
  -PassphraseFile D:\RedClawDebugBridge\passphrase.txt `
  -BindAddress <HOST_PHYSICAL_IPV4> `
  -NetworkFingerprint <LOCAL_NETWORK_TOKEN>
```

After `debug-bridge-host.sealed` appears, record its SHA-256:

```powershell
Get-FileHash -Algorithm SHA256 `
  D:\RedClawDebugBridge\signal\debug-bridge-host.sealed
```

Transfer only that sealed file to the Controller through an operator-approved
private channel. Verify the received SHA-256, place it in the Controller signal
directory, and start the Controller:

```powershell
.\scripts\service\start-debug-bridge.ps1 `
  -Role controller `
  -SignalDirectory D:\RedClawDebugBridge\signal `
  -BridgeId RCD-DBG-01 `
  -SessionEpoch epoch-20260901-01 `
  -PassphraseFile D:\RedClawDebugBridge\passphrase.txt `
  -BindAddress <CONTROLLER_PHYSICAL_IPV4> `
  -NetworkFingerprint <LOCAL_NETWORK_TOKEN>
```

Transfer `debug-bridge-controller.sealed` back to the Host in the same way and
verify its SHA-256 before use. If trickled candidates change before ICE opens,
repeat only the sealed-file transfer for the same epoch. Do not restart both
endpoints repeatedly, and never commit either sealed file.

The first sealed offer or answer may intentionally contain zero candidates
when `trickle_ice=true`. This is a valid description-first state, not a failed
signal. A later sealed revision must retain the same epoch and description and
add the gathered candidates. If connection stalls, capture both endpoints'
`local_description_ready`, `local_description_is_offer`,
`local_description_role_matches`, `local_has_ice_ufrag`,
`local_has_ice_pwd`, `local_has_fingerprint`, `local_candidate_count`,
`local_signal_publish_total`, `local_signal_publish_state`, and categorized
`last_signal_publish_error`.

Interpret a zero publication count before changing ICE settings:

- `description_role_mismatch`: Host did not produce an Offer or Controller did
  not produce an Answer. Verify that the build contains explicit negotiation
  ownership (`disableAutoNegotiation=true`) before changing ICE/STUN settings;
  a Controller Offer is an offer/answer state-machine defect, not a candidate
  reachability failure.
- `missing_ice_ufrag`, `missing_ice_pwd`, or `missing_fingerprint`: the local
  SDP is incomplete or unsupported. Do not invent a replacement value.
- `seal_failed`: the bounded authenticated signaling record was rejected.
- `write_failed`: the local sealed signal could not be atomically published.
- `ready_to_publish` lasting beyond one 50 ms bridge loop is a defect; preserve
  the typed status and bounded stderr for diagnosis.

## Control and Agent checks

```powershell
.\scripts\service\invoke-debug-bridge.ps1 -Action bridge_status -Json
.\scripts\service\invoke-debug-bridge.ps1 -Action remote_bridge_status -Json
.\scripts\service\invoke-debug-bridge.ps1 -Action status -Json
.\scripts\service\invoke-debug-bridge.ps1 -Action tail_log -Limit 100 -Json
.\scripts\service\invoke-debug-bridge.ps1 -Action export_evidence -TimeoutMs 60000 -Json
```

For a real Agent task, first inspect typed capability/project events, then use
an opaque registered project ID:

```powershell
.\scripts\service\invoke-debug-bridge.ps1 `
  -Action agent_task_create `
  -TaskId task-debug-01 `
  -ProjectId <OPAQUE_PROJECT_ID> `
  -Provider codex `
  -WorkDirectoryMode isolated_worktree `
  -Instruction 'Inspect this project and report the next safe integration step.' `
  -Json

.\scripts\service\invoke-debug-bridge.ps1 -Action agent_events -Json
```

Use `agent_approval` for a pending Codex approval or Cursor turn preapproval.
No script action accepts arbitrary shell text.

## Pass criteria

- local and remote status show `ice_connected=true` and `channel_open=true`;
- both endpoints report `local_description_ready=true`,
  `local_description_role_matches=true`, all three `local_has_*` fields true,
  and `local_signal_publish_total>0`; a zero initial candidate count is
  acceptable only while a later same-epoch sealed revision is still expected;
- a remote typed status returns from the peer RedClaw Debug Control pipe;
- unchanged-network A/B uses a new epoch and fresh sealed SDP but reports a
  route-cache hit; a different network fingerprint reports a miss;
- tampered/stale signals, unknown actions, replay, and absent Agent
  authorization fail closed;
- formal public-DHT GUI validation still proves same SHA, real media counters,
  all required channels, remote logs, and dual-endpoint evidence. The debug
  bridge connection alone is not a P0 pass.
