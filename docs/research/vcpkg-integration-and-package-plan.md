# vcpkg Integration and Package Plan (VS2022)

Date: 2026-03-13

## Conclusion (Short)
Yes, integrate vcpkg now before deeper feature coding.
The local machine already has a strong `x64-windows` package base, which can reduce implementation time significantly.

## Local vcpkg Status
- Root: `<LOCAL_VCPKG_ROOT>`
- vcpkg version: `2023-12-12-1c9ec1978a6b0c2b39c9e9554a96e3e275f7556e`
- Triplet observed in installed packages: `x64-windows`

## Already Installed and Immediately Reusable
- RTC/media/security/network:
  - `ffmpeg:x64-windows`
  - `libsrtp:x64-windows`
  - `openssl:x64-windows`
  - `c-ares:x64-windows`
  - `sdl2:x64-windows`
- Protocol/config/data:
  - `protobuf:x64-windows`
  - `yaml-cpp:x64-windows`
  - `sqlite3:x64-windows`
  - `boost-json:x64-windows`
- Utilities:
  - `zlib:x64-windows`
  - `zstd:x64-windows`
  - `boost-log:x64-windows`
  - `boost-asio:x64-windows`

## High-Value Packages to Add
These will reduce custom code in current roadmap modules:
- `libdatachannel` (+ feature `[srtp]`): M01 direct WebRTC/ICE/datachannel baseline.
- `nlohmann-json` (or keep `boost-json`): M02 schema serialization ergonomics.
- `spdlog` + `fmt`: M10 structured logging and diagnostics with lower friction than Boost.Log.
- `gtest`: stable unit test framework for protocol/session modules.
- `libqrencode`: M02 QR payload output helper.

Optional later:
- `simdjson`: if high-throughput JSON parsing becomes a bottleneck.
- `libsodium`: if we want modern high-level crypto APIs over OpenSSL primitives.

## Module-to-Library Mapping (Recommended)
- M01 Connectivity Engine:
  - Primary: `libdatachannel[srtp]`
  - Keep `c-ares` for DNS/stun host resolution paths if needed.
- M02 Offline Signaling Exchange:
  - `boost-json` (already installed) or `nlohmann-json`.
  - `libqrencode` for QR generation.
- M03 Secure Session and Identity:
  - `openssl` for AEAD/HKDF/EC primitives.
- M04/M05 Capture/Encode/Decode/Render:
  - `ffmpeg` for codec/mux/decode pipeline.
  - `sdl2` optional for early render harness.
- M08 File transfer/checksum/compression:
  - `zstd` + `openssl` digest/HMAC helpers.
- M10 Diagnostics and logging:
  - `spdlog` + `fmt` (recommended) or fallback `boost-log` already present.

## Integration Mode for This Project
Use CMake toolchain + manifest mode per-repo:
1. Add `vcpkg.json` to repo root (pin dependencies for reproducible builds).
2. Add toolchain in CMake preset:
  - `CMAKE_TOOLCHAIN_FILE=$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`
3. Keep `x64-windows` triplet initially to match current local package base.

## Suggested First Manifest Set
For immediate next milestones (A02/M02/M03/M09/M10):
- `openssl`
- `boost-json`
- `gtest`
- `spdlog`
- `fmt`
- `libqrencode`

For M01 kickoff:
- `libdatachannel` with `srtp` feature

## Suggested Installation Commands (if missing)
```powershell
vcpkg install openssl:x64-windows boost-json:x64-windows gtest:x64-windows spdlog:x64-windows fmt:x64-windows libqrencode:x64-windows
vcpkg install libdatachannel[srtp]:x64-windows
```

## Notes / Risks
- Current vcpkg tree suggests older baseline; update your local `VCPKG_ROOT` repository to expose newer ports and fixes.
- `x64-windows` is dynamic CRT/link style by default; if static deployment is needed later, consider `x64-windows-static` and rebuild.
- `libdatachannel` on Windows should be validated early with a tiny peer-loopback test to avoid integration surprises.
