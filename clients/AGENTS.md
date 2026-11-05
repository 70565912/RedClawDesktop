# Clients Directory Agent Guide

Applies to `clients/**`.

## Scope

This directory is for portable companion clients and planning artifacts.

Current boundary:

- Android phone/tablet companion is the first implementation path.
- mac portable and web mini access are planning-only unless explicitly re-scoped.
- Portable clients connect to the developer's own remote development machine.

## Android Controller Spike Exception

`clients/android-controller-spike/` is an explicitly scoped tech spike (M11-S0.x): an Android *Controller* in Kotlin + NDK that reuses `src/controller_core`. It may receive the desktop stream and, from S0.4, send touch/soft-keyboard input through the existing input lease/authorization gates. It must not open terminal, Agent or file-transfer channels and is not the M11 Flutter companion. The constraints below continue to apply to the companion client.

## Product Constraints

- Do not implement controlled keyboard/mouse input in the mobile companion unless the product scope changes explicitly.
- Supported first-slice capabilities are status/log/artifact visibility and guarded command presets.
- Maintain security-by-default and least-capability behavior. Any command trigger must be explicit, auditable, and constrained.

## Android Direction

- Recommended stack is Flutter for Android-first delivery.
- Use platform channels only for Android-specific integrations that cannot be expressed cleanly in shared Flutter code.
- Keep protocol/session model aligned with native `src/session`, `src/protocol`, and `src/security` contracts.

## Planning Artifacts

- Keep `clients/plans/` documents clearly labeled as planning-only.
- If a plan graduates to implementation, update root `README.md`, `docs/README.md`, and relevant runtime/module docs.
