# Android Controller tech spike (S0.1)

**Spike, not production.** Scope is locked to an Android *Controller*: connect
to a RedClaw Host by machine code + connection password, receive the real
encoded desktop stream, decode it with MediaCodec, and prepare the
touch/soft-keyboard input path. Out of scope: Host role, the M11 Flutter
companion, remote terminal, remote Agent, file transfer (the transfer channel
stays unused), TURN, store packaging and UI polish. The formal Android MVP is
gated on this spike passing S0.2–S0.5 below.

## Layout

| Path | Role |
| --- | --- |
| `src/controller_core/` | Portable Controller session core (`redclaw_controller_core`), shared with the Windows build. |
| `native/CMakeLists.txt` | NDK entry: builds the repo with `REDCLAW_CONTROLLER_CORE_ONLY=ON` plus the JNI shim. |
| `native/vcpkg.json` | Android-only dependency manifest (no ffmpeg/opus/Qt). |
| `native/jni/` | JNI bridge (`libredclaw_controller_jni.so`). |
| `native/build-android-native.sh` | Gradle-free arm64-v8a build used for CI/local verification. |
| `app/` | Kotlin shell: form, status line, `SurfaceView` + async `MediaCodec` sink. |

## What the core reuses (no second protocol implementation)

`redclaw_controller_core` only composes existing module contracts:

- DHT rendezvous: `service::DhtRendezvousClient`, `LibtorrentDhtRendezvousStore`
  wrapped in `IndirectDhtRendezvousStore` + file mailbox, exactly as the
  Windows runtime; topic/pairing secret derived from the machine code.
- Negotiation: `service::ConnectionNegotiationCoordinator`,
  `DhtPublicationTransaction`, `plan_dht_initial_snapshot`,
  persistent-offer adoption (fresh Controller adopts the Host standby offer).
- ICE/DataChannel + connection auth: `net::IceConnectivityWrapper` with
  `requireConnectionAuthentication` (SCRAM-style password proof over the
  control channel; channels are exposed only after auth is accepted).
- Control plane: `protocol::StreamControlMessageV1` (Hello/Capabilities,
  Ping/Pong, ViewportRequest, KeyframeRequest, MediaTransportFeedback) with
  `StreamControlEpochGuardV1`.
- Media: `net::parse_encoded_video_fragment`, `EncodedVideoFrameReassembler`,
  `MediaTransportFeedbackRecorder`. Complete Annex-B access units are handed
  to Kotlin as a direct `ByteBuffer` (one copy, into the MediaCodec input
  buffer); no Kotlin-side frame queue, latest-frame drop + IDR request on
  backpressure.

The Controller advertises only `viewport,media-transport-feedback,keyframe-recovery`
and capability versions 0 for terminal/transfer/clipboard/audio, so the Host
does not open those channels.

Not yet extracted (still inline in `src/main.cpp` `run_runtime_mode`):
ICE-failure repair / automatic reconnect, Host restart re-adoption, receiver
network stats, decoder-recovery coordinator, input lease. The Windows
Controller does **not** use `redclaw_controller_core` yet; moving it over is a
follow-up once the Android path proves the contract (see S0.3).

## Build

Prerequisites (Linux or macOS host; Windows works with equivalent paths):

- Android SDK with platform 34, build-tools 34, NDK `27.2.12479018`, CMake `3.22.1`.
- A vcpkg checkout (`VCPKG_ROOT`). First dependency build for `arm64-android`
  (OpenSSL, protobuf+host protoc, boost-json, zstd, libdatachannel[srtp],
  libtorrent, miniupnpc) takes roughly 30–40 minutes on 4 cores; reuse it via
  `VCPKG_INSTALLED_DIR`.
- A host C/C++ toolchain with libstdc++ (vcpkg builds host `protoc`).
- JDK 17+ and Gradle 8.7+ (no wrapper is committed; Android Studio's bundled Gradle also works).

Native library only (what the cloud agent verified):

```bash
export ANDROID_NDK_HOME=$HOME/android-sdk/ndk/27.2.12479018
export VCPKG_ROOT=$HOME/vcpkg
export VCPKG_INSTALLED_DIR=$HOME/rc-android-vcpkg-installed   # optional cache
clients/android-controller-spike/native/build-android-native.sh
# -> native/build/arm64-v8a/libredclaw_controller_jni.so (ELF64 AArch64)
```

APK (debug; verified with Gradle 8.9 on Linux, ~37 MB with unstripped debug `.so`):

```bash
cd clients/android-controller-spike
echo "sdk.dir=$HOME/android-sdk" > local.properties
gradle :app:assembleDebug \
  -Predclaw.vcpkgRoot=$VCPKG_ROOT \
  -Predclaw.vcpkgInstalledDir=$VCPKG_INSTALLED_DIR
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -s RedClawControllerCore RedClawDecoder
```

Gradle chains the vcpkg toolchain to the NDK toolchain
(`VCPKG_CHAINLOAD_TOOLCHAIN_FILE`), triplet `arm64-android`, `ANDROID_STL=c++_shared`
(matches the triplet's dynamic CRT), `minSdk 28` (the triplet's API level).

Windows/desktop builds are unaffected except that `redclaw_controller_core` is
now also built there (it links only `redclaw_net`/`service`/`security`/`protocol`).

## Host-side prerequisites still open

No Host behavior or wire format changed for S0.1 (shared-code edits are
build-only: CMake target selection and one libc++ type cast); the current Host already accepts a
Controller that speaks the existing v1 contracts. These gaps block later
stages and need explicit, capability-negotiated protocol work (both directions
of mixed-version compatibility must be tested per repo rules):

1. **Media header version negotiation.** The encoded-video fragment header has
   no Controller-advertised version; the Android core accepts whatever
   `parse_encoded_video_fragment` accepts today. Add an optional
   `media_fragment_version` capability before changing the header.
2. **Decoder capability report.** The Host picks H.264/HEVC and resolution
   without knowing the Controller's decoders. Add an optional
   Controller→Host capability (supported codecs/profiles, max resolution,
   low-latency support) from `MediaCodecList`; Host falls back to H.264 when
   absent. Until then HEVC-only Host settings may yield frames Android cannot decode.
3. **Unicode text input.** `RemoteInputEventV1` carries scan/virtual-key
   codes only; Android soft keyboards produce text/IME composition. Needs an
   optional text-input event + Host `SendInput(KEYEVENTF_UNICODE)` path behind
   the existing input lease/authorization gates (fail closed when absent).
4. **Touch → pointer mapping.** Normalized 0..65535 coordinates already exist
   (`normalized_x/y`); the spike computes them but does not send input until
   the input-lease handshake (`InputControlRequest`/`Status`) is ported.

## Remaining checks against RC7TST01 (coordinator, on the LAN)

Target Host: machine code `RC7TST01`, `DESKTOP-A3GG0TV`, bound `192.168.1.181`,
ICE UDP `55000`, Debug Host from `main@40b80cc`, `role=host phase=dht_waiting`.
Do not stop/restart the operator Host for these checks.

- **S0.2 LAN connect.** Phone on the same LAN, enter `RC7TST01` + password.
  Pass: phases `requesting → offer_applied → answer_published → ice_connecting
  → authenticating → streaming`; Host log shows Controller answer applied and
  `connection_auth accepted`; Host `fetch_hits>0`. Fail evidence to capture:
  `dht_fetch`/`remote_revision` counters, `controller_core skipped host record`
  observations, auth rejection reason.
- **S0.3 Media decode/display.** With Host `--stream-require-capture`:
  `frames`/`keyframes` advance, `decoded` advances, visible desktop on the
  SurfaceView. Record codec, size, decoded FPS over 30 s, dropped/kf_requests.
  Also port ICE repair/reconnect from `run_runtime_mode` (current core fails
  closed with `ice_transport_failed_reconnect_not_implemented`).
- **S0.4 Touch/soft-keyboard input.** Port input lease + `InputBatch`
  (pointer from normalized touch); add Unicode text event (Host prerequisite 3).
  Verify Host authorization gating and release-all on disconnect.
- **S0.5 Mixed-version + robustness.** Host on current release and on the
  Host build with new optional capabilities; Android with/without them;
  Host restart re-adoption; Wi-Fi↔cellular change; 10-minute continuity with
  bounded memory and no unbounded queues.

## Known limitations of S0.1

- One connection attempt per `ControllerSession`; no automatic recovery.
- No receiver network stats or playback-starvation reports, so Host
  adaptation relies on transport feedback only.
- Phone-side ICE port is fixed (default 55000) like the desktop runtime.
- `redclaw_controller_core_tests` (CTest) covers only config validation and
  fail-closed start. A Linux x64 smoke of `start()` (unregistered machine code)
  reached `requesting`, published the request to mainline DHT after ~20 s of
  bootstrap and stopped cleanly; the DHT/ICE/auth/media path is exercised by S0.2+ on
  real hardware. The reused modules keep their existing suites
  (`tests/cmake/service-tests.cmake`, `net-tests.cmake`, `protocol-tests.cmake`,
  `security-tests.cmake`).
