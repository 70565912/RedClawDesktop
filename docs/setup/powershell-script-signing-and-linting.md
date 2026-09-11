# PowerShell Script Signing and Linting

This guide defines the repository baseline for PowerShell quality and signature hygiene.

## Scope
- PowerShell scripts under `scripts/**`.
- Linting by `PSScriptAnalyzer` with repo settings in `PSScriptAnalyzerSettings.psd1`.
- Optional Authenticode signing for release/distribution workflows.

## Linting
Run analyzer with repository settings:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-powershell-script-lint.ps1
```

VS Code task (one-click):
- `Script: Lint (PowerShell)`

Fail threshold options:
- `Error`
- `Warning` (default)
- `Information`

Example:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-powershell-script-lint.ps1 -FailOnSeverity Error
```

## Signing
Sign scripts using a code-signing certificate thumbprint:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/sign-powershell-scripts.ps1 -CertificateThumbprint <THUMBPRINT>
```

Use LocalMachine certificate store if required:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/sign-powershell-scripts.ps1 -CertificateThumbprint <THUMBPRINT> -UseLocalMachineStore
```

## Signature Verification
Verify all scripts are signed and valid:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/verify-powershell-script-signatures.ps1
```

Allow unsigned scripts in local development:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/verify-powershell-script-signatures.ps1 -AllowUnsigned
```

VS Code task (local development mode):
- `Script: Verify Signatures (Dev)`

Combined VS Code task:
- `Script: Lint + Verify Signatures`

## Notes
- In this repository, signing is operationally required for release tracks, not mandatory for local development iteration.
- If `PSScriptAnalyzer` is missing, install with:

```powershell
Install-Module PSScriptAnalyzer -Scope CurrentUser
```
