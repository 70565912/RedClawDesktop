# P0-DS-10: P2P Debug Bridge v1

## Purpose and acceptance boundary

`redclaw_debug_bridge.exe` is a standalone, Windows-only recovery channel for
coordinating and diagnosing two RedClaw development machines when the normal
desktop rendezvous path cannot yet open `redclaw-control-v1` or
`redclaw-agent-v1`. It is diagnostic infrastructure, not a replacement for the
public-DHT, desktop-GUI, real-capture P0 acceptance path.

The bridge creates one reliable, ordered WebRTC DataChannel named
`redclaw-debug-bridge-v1`. It does not create media, normal control, or normal
Agent channels. ICE remains direct P2P and accepts STUN URLs only; TURN URLs are
rejected by the executable and launch script.

## Signaling and fast reconnect

Each run uses a new `session_epoch`, SDP, ICE username/password, certificate
fingerprint, and candidate generation. The two sealed files are
`debug-bridge-host.sealed` and `debug-bridge-controller.sealed`. They contain an
AES-GCM/PBKDF2 sealed `OfferBlobV1`. The passphrase is read from a local file and
must never enter Git, logs, or command output. When this recovery bridge is used
between machines, an operator transfers only the sealed files through an
approved private channel and verifies their SHA-256 values out of band. The
repository contains no signaling exchange area.

Debug Bridge sets the explicit `OfferBlobV1.trickle_ice` marker. This permits
the authenticated SDP description to be published with zero candidates, then
atomically republishes the same description as local candidates arrive. The
marker defaults to false, so existing sealed-file users still require at least
one bundled candidate. This prevents answer publication from deadlocking on a
platform where the local-description callback precedes candidate gathering.

RedClaw is the sole owner of WebRTC offer/answer timing. The shared ICE wrapper
disables libdatachannel automatic negotiation, emits an explicit Offer only on
the initiating Host, and emits an explicit Answer only after the Controller has
accepted the Host Offer. This is required for the public-DHT path that defers an
Answer until the cumulative candidate snapshot is ready. Optional Agent/Debug
Bridge channel creation also invokes the explicit Host offer path; when the
existing SCTP application section already covers the channel, the call is a
no-op rather than an independent automatic renegotiation.

A peer rejects a mismatched bridge ID, epoch, role nonce, expired blob, invalid
authentication tag, invalid SDP, or invalid candidate. After a successful
connection, the process stores a current-user DPAPI-protected route cache for
30 days. A cache is used only when its network fingerprint matches. It reuses
the bind/STUN profile and a narrow local port neighborhood, but never old SDP,
ICE credentials, fingerprints, or message epochs.

Local typed status exposes only bounded signaling diagnostics:
`local_description_ready`, `local_description_is_offer`,
`local_description_role_matches`, the boolean presence of ICE ufrag/password
and fingerprint, `local_candidate_count`, `local_signal_publish_total`,
`local_signal_publish_state`, and a categorized `last_signal_publish_error`.
The SDP attribute parser accepts ASCII case and surrounding-whitespace
differences but never substitutes or invents missing credentials. Role
mismatch and each missing attribute fail closed with a distinct category. The
status never exposes SDP, candidates, addresses, ICE credentials, fingerprints,
or filesystem paths.

## Command and Agent boundaries

The Controller exposes a current-user local named pipe through
`invoke-debug-bridge.ps1`. Remote RedClaw control is rebuilt from a strict
whitelist and forwarded to the existing Debug Control pipe. Unknown actions,
unknown fields, oversized messages, malformed values, old epochs, replays, and
out-of-order messages fail closed. There is no remote shell action.

When Host starts with both `--allow-agent-tasks` and a registered-project
manifest, typed `AgentMessageEnvelopeV1` frames are handled by the existing
`RemoteAgentBroker`. Natural-language task text is limited to 16 KiB and may
operate only in the selected registered project. Codex retains structured
approval; Cursor retains whole-turn preapproval. The bridge cannot enumerate or
take over unrelated local Agent sessions.

The bridge protocol is capped at 64 KiB with a 48 KiB payload. Controller Agent
history retains at most 256 typed frames. Bridge failure does not restart the
normal desktop connection. See
[the integration runbook](../testing/p2p-debug-bridge-integration-runbook.md).
