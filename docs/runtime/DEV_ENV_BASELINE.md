# Development Environment Baseline (Windows)

Date: 2026-03-13

## Detected on Current Machine
- OS: Windows
- Visual Studio: 2022 Professional (detected)
- Python: `python 3.13.12` (detected)
- GitHub CLI: `gh 2.88.1` (detected)
- Primary AI coding tool: GitHub Copilot
- Primary IDEs: Visual Studio Code, Visual Studio 2022
- Qt installed paths (provided by user):
  - `<LOCAL_QT_5_15_2_MSVC2019_64_PATH>`
  - `<LOCAL_QT_6_2_4_MSVC2019_64_PATH>`
- WDK: installed (user confirmed)
- .NET SDK/framework: installed (user confirmed)

## Gaps / Action Required
1. GitHub CLI auth and remote sync
- Current state: completed.
- Result: switched to SSH remote and push path is available.

2. CMake in PATH verification
- Current state: completed.
- Result: `cmake 4.2.3` resolves in shell PATH.

3. Ninja in PATH verification
- Current state: completed.
- Result: `ninja 1.13.2` installed via winget and resolves in shell after PATH reload.

4. MSVC v142 component identity check
- Current state: partially verified.
- `vswhere -requires Microsoft.VisualStudio.Component.VC.v142.x86.x64` returns no installation path.
- But `14.29.30133` toolchain binaries are present in VS2022:
  - `...\\VC\\Tools\\MSVC\\14.29.30133\\bin\\Hostx64\\x64\\cl.exe`
  - `...\\VC\\Tools\\MSVC\\14.29.30133\\bin\\Hostx64\\x64\\link.exe`
- Practical interpretation: v142-era compiler toolset appears available for local builds.
- Optional follow-up: if a project explicitly fails on `PlatformToolset=v142`, add the official VS component `MSVC v142 - VS 2019 C++ x64/x86 build tools`.

## Recommended Toolchain Decision (for this project)
- Language/runtime: C++17/20 + Qt (desktop native UI), Python only for tooling scripts.
- Compiler: MSVC via VS2022.
- Build system: CMake + Ninja.
- Qt baseline: keep current `msvc2019_64` kits for now, therefore require v142 component.
- Dependency manager: vcpkg manifest mode (root `vcpkg.json`) with toolchain `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`.
- Local path source of truth: `CMakeUserPresets.json` (gitignored, machine-local).

## Why this decision
- Aligns with remote desktop native performance goals.
- Matches installed WDK/VS ecosystem on host machine.
- Supports multi-agent deterministic builds via CMake presets.

## Install Checklist
- [x] `gh auth login` completed.
- [x] CMake installed and available in PATH.
- [x] Ninja installed and available in PATH.
- [x] VS2022 has `14.29` (`v142-era`) compiler binaries available.
- [ ] `vswhere` component-id check for `Microsoft.VisualStudio.Component.VC.v142.x86.x64` is positive.

## Mobile/Tablet Companion Environment (M11)
- Decision: Flutter-first (see `docs/architecture/adr-mobile-client-tech-choice.md`).
- Not required now for desktop-core work (`M01`~`M10`).
- Install when starting `M11` implementation:
  - Flutter SDK (stable)
  - Android Studio (Android SDK + platform-tools + emulator)
  - JDK 17
- iOS build/signing is out-of-scope for this Windows host and requires a macOS machine with Xcode.

## GitHub Transport Baseline
- Preferred Git transport: SSH (`git@github.com:...`) to avoid unstable HTTPS TLS resets observed previously.

## Verification Commands
```powershell
gh auth status
cmake --version
ninja --version
vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.v142.x86.x64 -property installationPath
Test-NetConnection github.com -Port 443
Test-NetConnection api.github.com -Port 443
```

## Latest Verification Snapshot (2026-03-13)
- `cmake --version` -> `4.2.3`
- `ninja --version` -> `1.13.2`
- `vswhere ... -requires Microsoft.VisualStudio.Component.VC.v142.x86.x64` -> no result (component-id check pending)
- `...\\VC\\Tools\\MSVC\\14.29.30133\\bin\\Hostx64\\x64\\cl.exe` -> present
- `vcpkg install --triplet x64-windows` -> dependencies resolved for project manifest.
