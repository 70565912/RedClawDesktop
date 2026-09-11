# GitHub Survey: Similar Open Source Projects

Date: 2026-03-12
Scope: serverless-first remote desktop, P2P NAT traversal, offline signaling, unattended pre-login access.

## Summary
There are similar projects, but no mainstream project exactly matches all constraints at once (offline signaling as primary mode + no custom business server + unattended pre-login + cross-platform desktop control).

## Relevant Projects

1. rustdesk/rustdesk
- Relevance: High for remote desktop + NAT traversal + relay fallback + unattended workflows.
- Evidence observed: references to rendezvous/relay servers, NAT testing, direct/relay switching logic, service/server mode internals.
- Gap vs target: default architecture still depends on ID/rendezvous services for mainstream UX.

2. tigervnc/tigervnc
- Relevance: High for Windows service model and pre-login/session handling.
- Evidence observed: Win service registration/start/stop code paths, service process execution, desktop/session switching helpers.
- Gap vs target: not a modern WebRTC P2P stack and not offline signaling-first.

3. paullouisageneau/libdatachannel
- Relevance: High as building block for lightweight ICE/STUN/TURN and manual SDP exchange.
- Evidence observed: ICE/STUN/TURN support, local/remote description and candidate APIs, examples that support copy/paste style SDP exchange.
- Gap vs target: library only, not complete remote desktop product.

4. coturn/coturn
- Relevance: Required infrastructure candidate for TURN fallback.
- Evidence observed: TURN/STUN feature set and RFC support; production-grade relay server implementation.
- Gap vs target: infrastructure component only.

## Decision
- Reuse ideas and selected components from existing projects.
- Build a dedicated product tailored to:
  - Offline signaling first (QR/encrypted text blob)
  - No self-developed business signaling server dependency
  - P2P preferred, TURN fallback
  - Unattended pre-login access via OS service/daemon model

## Notes for Agents
- Do not attempt to fork and patch one existing project as MVP.
- Build modularly with clean interfaces so signaling modes and transport internals can evolve independently.
