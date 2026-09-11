# Module Kanban

Updated: 2026-09-12

This board tracks public project status. Detailed machine-specific evidence stays outside the repository. The current priority is the Windows two-machine product path and the `v0.1.0` Developer Preview.

Status values: `done`, `in-progress`, `planned`.

## M01 Capture

- `M01-T01` Windows Graphics Capture and Desktop Duplication backends — done.
- `M01-T02` Multi-display product controls — planned.

## M02 Codec

- `M02-T01` H.264/HEVC encode/decode pipeline with hardware selection — done.
- `M02-T02` Broader hardware/driver compatibility matrix — in-progress.

## M03 Security

- `M03-T01` Identity, protected local secrets, and trust-store baseline — done.
- `M03-T02` Public security review and hardening — planned.

## M04 Protocol

- `M04-T01` Typed Control/Media/Agent framing and bounded parsing — done.
- `M04-T02` Compatibility/version policy for later public releases — planned.

## M05 Input

- `M05-T01` Authorized ordinary-desktop keyboard and mouse input — done.
- `M05-T02` Wider DPI/rotation/multi-display validation — in-progress.

## M06 Session

- `M06-T01` Host/Controller lifecycle, reconnect, and latest-frame semantics — done.
- `M06-T02` Long-duration cross-site stability — planned.

## M07 Service

- `M07-T01` Windows service and pre-login handoff scaffolding — in-progress.
- `M07-T02` Signed unattended installer and upgrade — planned.

## M08 Diagnostics

- `M08-T01` Bounded status, redacted logs, and evidence export — done.
- `M08-T02` Public support-bundle workflow — planned.

## M09 Render

- `M09-T01` D3D11 decoded-surface presentation with bounded CPU fallback — done.
- `M09-T02` Broader GPU/driver validation — in-progress.

## M10 UI

- `M10-T01` Device-code Host/Controller GUI and playback workspace — done.
- `M10-T02` Accessibility, localization, and product polish — in-progress.

## M11 Portable Client

- `M11-T01` Android-first portable client architecture — planned.

## X00 Cross-module delivery

- `X00-T01` Real local dual-GUI desktop stream with three channels — done.
- `X00-T02` Fixed ICE UDP port, early reservation, and ICE-targeted UPnP — done.
- `X00-T03` `v0.1.0` clean public repository and portable prerelease — done.
- `X00-T04` One physical cross-LAN public-DHT/direct-NAT real-video checkpoint — done; remote input, real Provider, mixed-version, and TURN coverage remain open.
- `X00-T05` Expanded physical cross-LAN and TURN acceptance — planned.

## Release gate

The Developer Preview requires successful builds, non-E2E unit tests, focused ICE/UPnP regression tests, a clean portable archive, and a functional local two-GUI run. The product performance baseline remains visible but does not block feature delivery solely because a machine-specific target is missed.
