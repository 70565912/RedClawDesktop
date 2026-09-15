<div align="center">

# RedClawDesktop

**A Windows P2P remote desktop and remote Agent workspace for developers**

[简体中文](README.md) · [Download v0.1.1](https://github.com/70565912/RedClawDesktop/releases/tag/v0.1.1) · [Developer docs](docs/README.md) · [Issues](https://github.com/70565912/RedClawDesktop/issues)

[![Release](https://img.shields.io/badge/release-v0.1.1-blue)](https://github.com/70565912/RedClawDesktop/releases/tag/v0.1.1)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4?logo=windows)](https://github.com/70565912/RedClawDesktop/releases/tag/v0.1.1)
[![License](https://img.shields.io/badge/license-Apache--2.0-green)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](CMakeLists.txt)
[![Qt 6](https://img.shields.io/badge/Qt-6-41CD52?logo=qt)](https://www.qt.io/)

</div>

RedClawDesktop connects a developer to their own Windows workstation by device code. One GUI combines the live desktop, explicitly authorized keyboard and mouse input, and interaction with AI coding Agents running on the Host. Online DHT rendezvous and ICE/STUN/TURN negotiation are used to establish a direct P2P connection whenever possible.

> `v0.1.1` is a Windows x64 Developer Preview for evaluation and two-machine testing. It does not yet include a signed installer or complete coverage of every cross-site NAT and TURN combination.

![RedClawDesktop Controller showing the live desktop stream and Agent panel](docs/assets/redclaw-controller-desktop-stream.png)

## Why RedClawDesktop

- **Built for development work**: desktop video, remote input, and Agent output share one workspace.
- **Device-code connection**: the normal product flow never asks for the peer's IP address.
- **Direct first**: DHT provides online rendezvous while ICE/STUN/TURN handles connection negotiation and NAT traversal.
- **Real media path**: the Host captures and encodes the desktop; the Controller decodes and presents it. A diagnostic preview is not accepted as proof of the real path.
- **Explicit authority**: remote input, unattended operation, and Agent tasks are bounded by user authorization, leases, and registered projects.

## Current capabilities

| Capability | v0.1.1 status |
| --- | --- |
| Windows Host and Controller GUI | Available |
| Device code and public-DHT rendezvous | Available |
| ICE/STUN direct connection and configurable TURN | Available; network coverage is still expanding |
| WGC and Desktop Duplication capture | Available |
| H.264/HEVC codecs and D3D11 presentation | Available; backend depends on hardware and drivers |
| Authorized keyboard and mouse control | Available; secure-desktop cases fail closed |
| Control, Media, and Agent channels | Available |
| Codex and Cursor provider bridge | Available after Host-side setup and authorization |
| Signed Windows MSI and mobile clients | Planned |

## Quick start

1. Download `RedClawDesktop-windows-x64-v0.1.1.zip` and `SHA256SUMS.txt` from the [v0.1.1 Release](https://github.com/70565912/RedClawDesktop/releases/tag/v0.1.1).
2. Verify the archive:

   ```powershell
   Get-FileHash .\RedClawDesktop-windows-x64-v0.1.1.zip -Algorithm SHA256
   ```

3. Extract it into a new directory and run `redclaw_desktop.exe`. Supported Host and Controller versions negotiate their common capabilities; connecting does not require identical versions or binaries.
4. Select **Host** on the controlled PC and wait. Select **Controller** on the controlling PC, enter the Host device code, and connect.
5. Once video is visible, enable remote input explicitly. Agent features also require a configured provider and registered projects on the Host.

The portable archive does not install a Windows service. Windows SmartScreen may warn because this Developer Preview is not code-signed.

## ICE UDP port and router setup

- The default ICE UDP port is **55000** and can be changed in the GUI network settings.
- Two different machines can both use 55000. Two instances on one machine need different ports; the project test scripts use Host 55000 and Controller 55001.
- When UPnP is enabled, RedClawDesktop maps the actual ICE UDP port. A UPnP failure is visible in diagnostics and does not disable STUN, TURN, or ordinary hole punching.
- For a manual router rule, select **UDP** and use the configured ICE port for both the internal and external port.
- The DHT listening port is used only for rendezvous and does not need a manual mapping.

Direct connectivity depends on NAT, CGNAT, firewall, and ISP policy. Configure TURN when direct traversal is unavailable. `v0.1.1` has not yet been tested across every network combination.

## GitHub cloud validation boundary

GitHub Actions uses a newly provisioned Windows virtual machine for each job. Dependencies already installed in a developer's local vcpkg tree are not available in that VM. The unit workflow installs its own Qt, FFmpeg, and other dependencies during CMake configuration. Matching vcpkg revisions and dependency manifests reuse a hosted binary cache; a missing cache or a dependency change still requires one longer build.

After a prerelease is published, a separate package check downloads the final ZIP from GitHub Releases, verifies it against `SHA256SUMS.txt`, checks required portable files, and runs `redclaw_desktop.exe --help` from the archive. It does not rebuild the application; it detects damaged uploads, missing DLLs, and a broken command entry point.

The GitHub-hosted VM is not a real desktop acceptance environment. Desktop capture, hardware-backed D3D11 decode and presentation, UPnP, cross-site ICE traversal, and physical keyboard or mouse input remain local or physical two-machine checks. See [GitHub Actions validation](docs/testing/github-actions-validation.md) for the exact boundary.

## Security model

- Remote input is rejected when capability, authorization, lease, or lifecycle state is missing.
- Agents can work only inside projects registered by the Host; the protocol exposes no arbitrary remote shell.
- Diagnostics bound sensitive paths, addresses, and credentials. Runtime signaling, keys, and local configuration must never be committed.
- Local encrypted debug signaling files remain in operator-selected directories; the repository is not a communication or signaling exchange.

See the [solution architecture](docs/architecture/solution-architecture.md), [ordinary desktop input design](docs/architecture/ordinary-desktop-remote-input-v1.md), and [remote Agent bridge design](docs/architecture/remote-development-agent-bridge-v1.md).

## Build from source

Requirements: Windows 10/11 x64, Visual Studio 2022, CMake 3.20+, vcpkg, and Qt 6 Widgets. Copy `CMakeUserPresets.example.json` to the local-only `CMakeUserPresets.json`, set your dependency paths, then run:

```powershell
.\build.ps1
.\build.ps1 -Configuration Release
```

The standard entry point builds the project and stages a runnable tree under `release\Debug\` or `release\Release\`. Do not replace it with a bare `cmake --build` for the main application because the staged binaries may remain stale. See the [developer documentation index](docs/README.md).

## Project status

`v0.1.1` covers local dual-GUI validation of real capture, encode, transport, decode, and presentation plus the Control, Media, and Agent channels. Strict performance targets remain observed engineering metrics and do not block this functional preview; results depend on hardware, drivers, resolution, and network conditions.

The next phase expands cross-site and TURN coverage, hardens unattended install and upgrade, adds code signing, and continues reducing GUI scheduling cost during large Agent output. See [PROJECT_STATE.md](docs/runtime/PROJECT_STATE.md).

## Contributing and license

Issues and pull requests are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) and the [developer documentation index](docs/README.md). Report security issues privately according to [SECURITY.md](SECURITY.md).

RedClawDesktop is licensed under [Apache License 2.0](LICENSE). Distributed third-party components retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `third_party/licenses/`.
