# Scripts Directory Agent Guide

Applies to `scripts/**`.

## Scope

This directory contains PowerShell helpers for CMake guarding, service validation, evidence collection, script signing/linting, capture/session hardening, and runtime profile templates.

## PowerShell Rules

- Prefer parameterized scripts over hardcoded machine paths.
- Use `powershell.exe -NoProfile -ExecutionPolicy Bypass -File ...` or `pwsh -File ...` consistently with surrounding scripts.
- Keep output operator-readable, but never print secrets, passphrases, private tokens, or unredacted runtime configs.
- Prefer native PowerShell/CIM APIs for Windows service operations where practical.
- Admin-gated scripts must detect privilege requirements early and fail with actionable messages.
- Destructive actions must be idempotent, scoped to explicit paths, and safe on repeated runs.

## Linting and Signing

Lint scripts with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-powershell-script-lint.ps1 -FailOnSeverity Warning
```

Signature verification for local development:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/verify-powershell-script-signatures.ps1 -AllowUnsigned
```

Follow `docs/setup/powershell-script-signing-and-linting.md` and `PSScriptAnalyzerSettings.psd1`.

## Evidence and Reports

- JSON reports should have stable field names and explicit pass/fail fields.
- Write generated evidence under `build/reports/` or the task-specific run directory.
- Do not require reboot, admin, network, or dual-machine execution from default local validation unless the task explicitly calls for it.

## Runtime Profiles

- Template configs under `scripts/session/runtime-profile-*.template.conf` define user-editable runtime contracts.
- Keep parser behavior synchronized with `src/helper` runtime profile validation.
- When adding profile keys, update templates, parser tests, README/runtime docs, and GUI/CLI handoff behavior as needed.
