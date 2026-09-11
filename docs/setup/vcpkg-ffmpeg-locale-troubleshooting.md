# vcpkg FFmpeg localized MSVC output troubleshooting

On Windows, a localized MSVC banner can corrupt FFmpeg's generated `CC_IDENT` string in `config.h`. A separate failure occurs when the FFmpeg `gfxcapture` filter is enabled without the C++/WinRT headers.

## Symptoms

- `fatal error C1083` for `winrt/Windows.Graphics.Capture.h`
- `config.h(...): error C2001`
- a malformed or non-ASCII `CC_IDENT` value
- later CMake generator errors after the vcpkg install has already failed

Relevant logs normally live below:

- `build/<preset>/vcpkg-manifest-install.log`
- `<VCPKG_ROOT>/buildtrees/ffmpeg/`

## Diagnosis

1. Confirm that the selected triplet contains `include/winrt/Windows.Graphics.Capture.h`.
2. Inspect the generated FFmpeg `config.h`; a healthy identity is an ASCII string such as:

```c
#define CC_IDENT "msvc-cl"
```

3. Treat subsequent generator or compiler-detection errors as secondary until the vcpkg failure is resolved.

## Supported remedies

- Keep the repository's explicit `cppwinrt` dependency so required WinRT headers are installed.
- Use an English build-tools environment when rebuilding FFmpeg.
- Prefer a trusted team binary cache when contributors use different Windows locales.
- If an FFmpeg port patch is needed, maintain it as a reviewed vcpkg overlay in the repository. Do not depend on edits inside one workstation's vcpkg checkout.

For a binary-cache-only consumer, set these values in the local `CMakeUserPresets.json` or process environment:

```text
VCPKG_BINARY_SOURCES=clear;files,<team-cache-path>,read
VCPKG_INSTALL_OPTIONS=--only-binarycaching
```

Populate the cache from a verified build machine with a matching vcpkg baseline, triplet, feature set, and compiler toolset. A cache hit reports restored packages and avoids rebuilding FFmpeg locally.
