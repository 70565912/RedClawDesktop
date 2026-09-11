# Development Log

This public log records release-level changes. Machine-specific paths, addresses, runtime signaling, credentials, and raw evidence are intentionally excluded.

## 2026-09-11 — offsite desktop stream checkpoint

- Completed one physical cross-LAN Controller-to-Host run through public DHT and a direct NAT path. ICE connected, all four negotiated channels opened, and real H.264 capture, transport, hardware decode, and GUI presentation advanced without decode or present failures.
- This checkpoint covered the real video path. Remote input, a real Agent Provider, mixed-version peers, TURN fallback, and Release qualification remain separate acceptance work.

## 2026-09-11 — v0.1.0 public prerelease

- Published a clean public-repository baseline with Apache-2.0 licensing, bilingual product documentation, a sanitized GUI screenshot, release notes, and a Windows x64 portable-package publisher.
- Removed the repository-based communication exchange and its synchronization script. Debug Bridge retains local encrypted signaling files and explicit operator transfer; the runtime DHT local record store remains part of the rendezvous implementation.
- Changed the installer workflow to manual dispatch while the Windows service package remains unsigned and incomplete for public distribution.
- Fixed the Agent account Login action so its local command output, startup errors, non-zero exits, cancellation, timeout, and recovery guidance remain visible in the settings dialog.
- Defined functional package checks as release-blocking and kept hardware-sensitive performance targets as reported engineering observations.

## 2026-09-11 — ICE UDP port and UPnP ownership

- Added one `ice_udp_port` setting shared by CLI, runtime profiles, GUI settings, diagnostics, and PowerShell launch scripts. The product default is UDP 55000; local dual-process validation uses Controller 55001.
- Reserved the configured UDP port during startup, fixed the libdatachannel port range to that value, and made conflicts fail with an explicit error.
- Moved UPnP ownership from the DHT listener to the ICE session. Mapping failures stay visible without disabling STUN, TURN, or ordinary hole punching.
- Added focused tests for defaults, overrides, invalid values, propagation, fixed ICE ranges, port conflicts, and mapping state.

## Earlier development

The pre-public private repository contains the detailed implementation history and local acceptance evidence. The public repository starts from one sanitized root commit so internal coordination records and machine-specific evidence are not published.
