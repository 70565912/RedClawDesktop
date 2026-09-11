# Windows Installer Packaging Plan

Date: 2026-03-20
Status: scaffold-ready

## Objective
Create a dedicated installer engineering path for install and uninstall of the RedClaw host package with service lifecycle registration.

## Deliverables
- WiX source template for product/service install-uninstall flow.
- Build script for local MSI generation.
- CMake custom targets for installer planning and explicit build invocation.
- Release checklist integration (future).

## Current Scaffold
- Installer project root: `installer/`
- WiX product template: `installer/wix/Product.wxs`
- Build helper script: `installer/scripts/build-wix-installer.ps1`
- CI scaffold workflow: `.github/workflows/windows-installer-scaffold.yml`
- CMake targets:
  - `redclaw_installer_plan`
  - `redclaw_installer_build`

## Packaging Workflow (Planned)
1. Build host binaries with existing CMake presets.
2. Run installer build script:
   - `pwsh -File installer/scripts/build-wix-installer.ps1 -Configuration Release`
3. Validate MSI install/uninstall flow in clean Windows test environment.
4. Validate service state transitions and cleanup after uninstall.

## Pending Hardening
- Replace placeholder product metadata with release metadata policy.
- Add code-signing pipeline for MSI and binaries.
- Add upgrade/downgrade policy tests.
- Promote CI scaffold to release-grade pipeline (versioning, signing, provenance, retention policy).
- Integrate with M07 service wrapper and lifecycle validation matrix.

## M03 Trust-Store Env Override Smoke Validation

Use this smoke validation after building a service-capable host binary to verify service-level environment overrides are persisted and survive restart.

### Script
- `scripts/service/test-m03-trust-store-env-override.ps1`

### Required privileges
- Must run in elevated PowerShell (Administrator).
- Script creates and deletes a temporary Windows service by default.

### Recommended invocation
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/service/test-m03-trust-store-env-override.ps1 `
  -ServiceName RedClawDesktopM03Smoke `
  -HostServiceBinaryPath build/vs2022-x64/src/Debug/redclaw_desktop.exe `
  -Fallback clear `
  -OutputJsonPath build/reports/m03-trust-store-env-override-smoke.json
```

### CMake helper target
```powershell
cmake --build --preset debug-local --target redclaw_m03_trust_store_env_override_smoke
```

### Smoke assertions
1. Temporary service is created successfully.
2. `Environment` multi-string includes:
- `REDCLAW_TRUST_STORE_PATH=<path>`
- `REDCLAW_TRUST_STORE_FALLBACK=clear|retain`
3. Values remain unchanged after service stop/start cycle.
4. JSON report is emitted under `build/reports/`.

### Cleanup behavior
- Default: script stops and deletes the temporary service and keeps the JSON report.
- Optional: pass `-KeepServiceAfterRun` for manual post-checks.

### Notes
- If the selected binary is not a real Windows service executable, the start step will fail and the smoke result will be `FAIL`.
