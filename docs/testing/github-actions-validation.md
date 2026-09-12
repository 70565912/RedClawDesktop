# GitHub Actions validation

GitHub-hosted Windows runners provide repeatable build and package checks on a newly provisioned virtual machine. They complement local and physical two-machine testing; they do not replace the product acceptance environment.

## Why the first cloud build is slow

The unit workflow clones its own vcpkg checkout. During CMake configuration, manifest mode installs Qt, FFmpeg, libdatachannel, Boost, OpenSSL, and the remaining native dependencies into the runner workspace. A developer's local vcpkg installation and compiled packages are not available to that virtual machine.

The unit workflow stores compiled vcpkg packages in a GitHub Actions cache keyed by the Windows runner, vcpkg revision, manifest, configuration, and overlays. A matching later job can restore those binaries, while a cold cache or changed dependency input still performs the full dependency build. This is independent from the locally built portable package uploaded to GitHub Releases.

## Automated cloud checks

`ci-unit-tests.yml` configures the Windows build without optional libtorrent/miniupnpc backends, compiles the selected non-E2E unit targets, and runs CTest. The backend-independent DHT codec, publication, validation, and listen-port selection tests remain part of this baseline; physical public-DHT behavior is covered separately.

`release-package-smoke.yml` runs after a GitHub Release is published and can also be started manually for an existing tag. It:

1. checks out the exact release tag;
2. downloads the published Windows x64 ZIP and `SHA256SUMS.txt`;
3. verifies the archive SHA256 and rejects unsafe archive paths;
4. checks the portable package for the executable, Qt Windows plugin, README files, license, and release manifest;
5. executes `redclaw_desktop.exe --help` from the extracted archive and requires exit code zero plus the expected usage output.

This package smoke does not rebuild dependencies. It verifies the actual uploaded asset and detects archive damage, missing load-time DLLs, or a broken command entry point.

## Physical acceptance boundary

The standard hosted runner is not accepted as evidence for:

- real Desktop Duplication or WGC capture from an operator desktop;
- hardware-backed D3D11 decode, swap-chain presentation, or performance baselines;
- router-controlled UPnP mapping;
- cross-site ICE behavior across two physical networks;
- focus-sensitive native keyboard and mouse injection;
- a real local Agent Provider login or approval flow.

Use the local dual-GUI script and the physical cross-LAN playbooks for those checks. A self-hosted runner may orchestrate trusted machines, but the public repository must never execute untrusted pull-request code on a development workstation.

## Release interpretation

A passing package smoke means the published archive is intact and its command entry loads on a clean Windows runner. It does not mean that desktop video, input, GPU acceleration, NAT traversal, or Provider integration passed. Release notes must list local, physical, and cloud evidence separately.
