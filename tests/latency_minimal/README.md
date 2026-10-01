# T29鈥揟31 focused Linux checks

This test-only CMake project compiles repository production sources. It does not
build, deploy, restart, or run the desktop application and opens no network socket.
Linux software-codec and condition-variable evidence does not qualify Windows
DDA/QSV/NVENC, Windows native timers, real NAT/TURN, or end-to-end latency.

## Requirements and reference invocation

C++20 compiler, CMake, Threads and GoogleTest are required. The default codec check
also requires the repository protocol/render/capture dependencies: OpenSSL,
Protobuf/protoc, zstd, Boost.JSON, Opus, FFmpeg avcodec/avutil/swscale/swresample
(including a functioning software H.264 encoder). Use existing legal dependency
provisioning; this harness never installs packages. Prefix/sysroot pkg-config
settings must describe the executor's actual dependency location.

From the repository root, a reference developer invocation is:

```sh
cmake -S tests/latency_minimal -B build/latency-minimal -DCMAKE_BUILD_TYPE=Debug
cmake --build build/latency-minimal --parallel 4
ctest --test-dir build/latency-minimal --output-on-failure --no-tests=error --repeat until-fail:3
```

The executing Agent owns its methods and local rules. Run serialized build/test
operations and retain full source/test/binary hashes and raw XML locally. The
codec-off option supports an independent transport check if codec dependencies
are unavailable; it is not a T29 pass. There are no skipped placeholders.

## Acceptance results

- **T29:** existing `DesktopBudgetSurvivesLowCadenceResizeAndReconnect` uses the
  production software encoder and decoder. Four sessions submit 20 frames total:
  1184x666 at requested 30 FPS, 1778x1000 at 5 FPS, 1184x666 at 1 FPS, then same-size
  reconnect at 5 FPS. At every frame assert nominal 30 FPS, time base 1/30, GOP 60,
  bitrate 1892/4267 by geometry, no rate-update attempts, strictly increasing PTS,
  nonempty encoded packets, and decoded dimensions. The frame pattern is synthetic
  codec input, explicitly not desktop-capture evidence.
- **T30:** three existing production-pacer cases exercise ACK, external expiry,
  absent refresh, autonomous expiry, multiple packet expiries and concurrent
  budget/reset rejection. The absent-refresh negative control must drop; the
  refresh arm must send without fabricating an ACK or loss. Expected negative
  control drops are not runtime failures.
- **T31:** three bounded cases hold the condition-variable predicate mutex for
  80 ms or delay a pacer transport-state/send callback by 80 ms. Assertions and XML
  properties distinguish the trace buckets and require zero in-flight wait and
  overflow for callback injections. They establish the current trace's attribution
  limits, not the historical 6.215-second gap's cause.

Return only task outcome, actual repetitions/case/frame counts, failure/skips,
codec-contract results, relevant timing properties, source/patch identity and an
opaque evidence reference. Do not return raw logs, paths, commands, account,
permission, process or credential details. Do not infer latency by comparing two
machines' clocks. Stop when the finite repetitions finish.
