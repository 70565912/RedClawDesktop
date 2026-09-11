# Installer Project (Windows)

This folder contains the installer engineering scaffold for RedClaw Desktop host setup and removal.

## Scope

- Build a Windows installer package for host deployment.
- Register and unregister `RedClawHostService` during install/uninstall.
- Keep uninstall idempotent and fail-closed for privileged operations.
- This installer is a required product deliverable for service-mode deployment; manual binary copy is development-only.

## Structure

- `wix/Product.wxs`: WiX v4 product template (service install/uninstall actions).
- `scripts/build-wix-installer.ps1`: local build helper for MSI generation.
- `CMakeLists.txt`: optional custom targets for installer tasks in CMake workspace.
- `.github/workflows/windows-installer-scaffold.yml`: CI scaffold to build host binary and produce MSI artifact.

## Current Status

- Planning and scaffold ready.
- Real product metadata, signing pipeline, upgrade code strategy, and release CI wiring are pending.
- Installer completion is blocking for production-style host deployment acceptance.

## Local Build (planned baseline)

```powershell
pwsh -File installer/scripts/build-wix-installer.ps1 -Configuration Release
```

## Notes

- This scaffold assumes WiX Toolset v4 CLI (`wix`) is available in PATH.
- The installer build helper now stages a runtime publish directory under `build/installer/runtime/<Configuration>` by calling `scripts/service/publish-desktop-release.ps1`, then packages `redclaw_host_service.exe` plus companion runtime DLLs from that staged directory.
- `-BuildOutputDirectory` can be used to point the installer build at a non-default build tree such as `build/ci-installer/src/Release`.

## CI Scaffold

- Workflow file: `.github/workflows/windows-installer-scaffold.yml`.
- Trigger: manual dispatch or push affecting installer/build inputs.
- Output: MSI uploaded as workflow artifact `redclaw-windows-installer-msi`.
