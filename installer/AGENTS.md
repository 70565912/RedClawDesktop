# Installer Directory Agent Guide

Applies to `installer/**`.

## Scope

The installer directory owns Windows host deployment packaging:

- WiX v4 product template under `wix/Product.wxs`.
- Local MSI build helper under `scripts/build-wix-installer.ps1`.
- Optional CMake installer targets.

## Installer Rules

- Installer work should support service-mode deployment of `redclaw_host_service.exe`.
- Register/start/stop/uninstall behavior must be idempotent and fail closed for privileged operations.
- Uninstall should clean service registration without assuming manually copied binaries.
- Do not hardcode developer machine paths.
- Preserve a development path where binaries can still be built and tested outside the installer.

## Build Baseline

Planned local command shape:

```powershell
pwsh -File installer/scripts/build-wix-installer.ps1 -Configuration Release -HostServiceBinaryPath build/vs2022-x64/src/Release/redclaw_host_service.exe
```

Assume WiX Toolset v4 CLI `wix` is required in PATH for MSI generation.

## Documentation and CI

- Update `installer/README.md` for installer behavior, prerequisites, and known limitations.
- Keep `.github/workflows/windows-installer-scaffold.yml` aligned if packaging behavior changes.
- Do not add release signing or publishing steps without explicit task scope.
