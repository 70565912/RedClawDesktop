# Project State

Updated: 2026-09-12

## Current release

RedClawDesktop `v0.1.0` is the first Windows x64 Developer Preview. Its scope is the GUI Host/Controller workflow, public-DHT rendezvous, ICE connectivity, real desktop capture and codec path, D3D11 presentation, explicitly authorized input, and the optional remote Agent channel.

The portable ZIP is the only binary distribution for this release. The service MSI remains an unsigned development scaffold and is excluded from the release.

## Verified local baseline

- Debug and Release builds use `build.ps1` so the matching app-local runtime is staged under `release/<Configuration>`.
- The local two-GUI path has exercised real capture, encode, transport, decode, presentation, authorized input, and Control/Media/Agent channel activity.
- The default ICE UDP port is 55000. GUI, CLI, runtime profiles, and integration scripts propagate the same setting. Local two-process tests use Controller 55001.
- UPnP targets the configured ICE UDP port. DHT keeps its independent listening port and does not request a router mapping.
- Startup reserves the configured ICE port so an occupied port fails early instead of silently changing the runtime contract.

## Verified cross-LAN checkpoint

One physical Controller-to-Host Debug run connected through public DHT and a direct NAT path. ICE reached connected, the negotiated channels opened, and real H.264 receive, hardware decode, and GUI presentation counters advanced without decode or presentation failures. The detailed machine-specific evidence remains in the private archive. This checkpoint did not cover remote input, a real Agent Provider, mixed-version interoperability, TURN fallback, or Release qualification.

## Acceptance policy

Functional correctness, channel continuity, real media progression, input authorization, package integrity, and absence of decode/presentation failures remain release requirements. Strict performance targets are engineering observations because results vary by hardware, driver, resolution, and network. A measured code regression still requires investigation, but an unmet aspirational threshold alone does not block feature development or this Developer Preview.

## Open work

1. Expand physical cross-LAN coverage across additional NAT types and validate configured TURN fallback.
2. Harden, sign, and validate the unattended service installer before publishing an MSI.
3. Continue GUI scheduling and large Agent-output optimization against the product performance baseline.
4. Resume Android-first portable client work after the Windows connection flow is stable.

## Operator boundary

The repository is not a communication or signaling exchange. Cross-machine coordination uses the established Control/Agent channels when connected and explicit operator actions during recovery. Runtime signaling, encrypted blobs, local paths, credentials, and evidence archives remain outside Git.

## Resume point

After `v0.1.0`, continue the cross-LAN playbook with independently recorded versions, negotiated capabilities, artifact hashes, Host ICE UDP 55000, and runtime evidence. Supported versions must interoperate through their common capability set. Treat a connected state as preliminary until real capture, receive, decode, and presentation counters advance.
