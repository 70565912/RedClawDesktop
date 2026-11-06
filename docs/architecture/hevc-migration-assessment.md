# H.265/HEVC 迁移评估 / HEVC Migration Assessment

Status: assessment only（仅评估，未改变运行时行为）. Baseline: `main` @ `40b80cc`, 2026-11-06.
Scope: Windows Host → Windows Controller desktop stream; Android Controller spike（draft PR #2）implications included.
所有结论均为静态代码审计，未运行 HEVC 实测；码率、延迟、CPU/GPU 数值均为待验证假设。

## 1. 结论 / Verdict

- **No-go for "switch H.264 → HEVC by default" now.** 直接把默认值改成 HEVC 会在没有任何解码能力信息的情况下发送 HEVC，并且当前码率预算与编解码器无关，**不会降低码率**（见 §3.1）。
- **Go for phased, optional HEVC**: H.264 保持默认；先加 Controller 解码能力上报（typed, optional field），Host 仅在双方都声明支持且本机硬件 HEVC 编码可用时选择 HEVC，否则回落 H.264。
- 大部分编解码、分片、解码管线已经存在 HEVC 分支，工作量集中在 **协商、编码器选择/回落、HEVC 码率预算、关键帧恢复核验、测试矩阵**，而非新建管线。

## 2. 当前链路 / Current path (verified)

| 阶段 Stage | 现状 Today | 代码 Code |
| --- | --- | --- |
| 编码器选择 Codec choice | 运行时三处硬编码 H.264，无 CLI/GUI/profile 设置 | `src/main.cpp` `profile_request.preferred_codec = EncoderCodec::kH264`（~L6516）、`bridge_request.codec = EncoderCodec::kH264`（~L6535）、自适应码率 `adaptive_profile_request.preferred_codec`（~L8815） |
| 枚举 Enums | `EncoderCodec{kH264,kHevc}`；`EncodedVideoCodec{kUnknown,kH264,kHevc}` | `src/capture/include/redclaw/capture/capture_module.h`、`src/render/include/redclaw/render/render_module.h` |
| 编码后端 Encoders | FFmpeg libavcodec。HEVC 候选：`hevc_nvenc` / `hevc_qsv` / `hevc_amf` / 软件 `libx265`；H.264：`h264_*` / `libx264` / `libopenh264` | `resolve_encoder_candidates()` in `src/capture/src/capture_module.cpp` |
| 依赖 Deps | vcpkg `ffmpeg[x264,x265,qsv,nvcodec,amf]`；发布包已带 x265（GPL-2.0+） | `vcpkg.json`, `THIRD_PARTY_NOTICES.md` |
| 低延迟参数 Low-latency | CBR、B-frames=0（且 open 后强校验 `max_b_frames==0`）、无 lookahead、`AV_CODEC_FLAG_LOW_DELAY`、repeat headers/Annex B；NVENC `p1/ull/zerolatency/delay=0` + 强制核验 `forced-idr=1`；QSV `async_depth=1`、`forced_idr=1`；AMF `ultralowlatency`；x264/x265 `zerolatency/ultrafast` | `build_low_latency_encoder_profile()`、encoder open 段（~L2405–L2490） |
| 码率预算 Budget | 约 0.08 bpp @ 名义 30 FPS，CBR，max=1.2×target，GOP=2×fps（60） | `build_low_latency_encoder_profile()` / `build_desktop_encoder_profile()` |
| 关键帧识别 Keyframe | `AV_PKT_FLAG_KEY` 或 NAL 扫描；H.264 NAL 5；HEVC NAL 16–21（BLA/IDR/CRA） | `payload_contains_hevc_keyframe()`、`packet_contains_keyframe()` |
| 线格式 Wire | 视频分片头带 1 字节 codec：`1=H.264`、`2=HEVC`；`0` 拒收；分片头版本固定 `kFragmentVersion` | `src/net/src/video_frame_transport.cpp`、`encoded_video_codec_to_binary/from_binary()` in `src/main.cpp` |
| 重组 Reassembly | 每帧校验 codec 一致；丢帧后 `awaiting_keyframe_` 直到下一个 keyframe | `EncodedVideoFrameReassembler::push()` |
| 本地 GUI 通道 Shared memory | `kDirectFrameFormatH264` / `kDirectFrameFormatHevc=3` | `src/helper/include/redclaw/helper/direct_frame_shared_memory.h`、`write_encoded_frame()` |
| Controller 解码 Decode | FFmpeg 解码，硬件优先级 D3D11VA → QSV → CUDA → DXVA2，失败回落软件；codec 改变会重建解码器 | `ensure_decoder()` / `resolve_decoder_hw_device_priority_order()` in `src/render/src/render_module.cpp`；`direct_frame_receiver.cpp` |
| 软解回落 SW fallback | 仅在 **尚未成功解出任何帧** 时触发（`decoded_frames == 0`） | `should_fallback_to_software_decode()` |
| 能力协商 Negotiation | Hello/Capabilities 有可选版本字段（terminal/audio/capture-status…）与 feature 字符串，**没有视频编解码能力字段** | `StreamControlMessageV1` in `src/protocol/include/redclaw/protocol/stream_control_protocol.h`；`src/main.cpp` Hello 处理（~L4682） |
| 测试 Tests | 所有 encoder/roundtrip 测试只用 `kH264`；HEVC 仅有枚举映射，无编码/解码用例 | `tests/capture/encoder_execution_session_tests.cpp`、`tests/integration/ffmpeg_roundtrip_integration_tests.cpp` |
| 看板 Kanban | `M02-T01 H.264/HEVC … done` 实际只代表 HEVC 管线代码存在，未被运行时选择或测试 | `docs/runtime/MODULE_KANBAN.md` |

补充事实 / Additional facts:

- 硬件能力探测把 H.264/HEVC 混为一项：`nvenc_available = has_any_encoder_named({"h264_nvenc","hevc_nvenc"})`（QSV/AMF 同理）。FFmpeg 中存在编码器名 ≠ 该 GPU/驱动支持 HEVC 编码。
- 后端回落顺序：首选硬件不可用 → 其他硬件 → 软件；打开失败也会落到 `kSoftware`。对 HEVC 意味着可能落到 `libx265`。
- Android spike（PR #2，未合并，本任务不修改）`MediaCodecVideoSink.kt` 已把 codec `1→video/avc`、`2→video/hevc`；spike README 已列出“Controller 上报 MediaCodecList 能力，否则 Host 默认 H.264”。

## 3. HEVC 需要改什么 / What must change

### 3.1 编码与设置 Encode & settings
1. 用一个有归属的 typed 设置替换三处硬编码（例如 `stream_video_codec = auto|h264|hevc`，默认 `h264`），CLI / runtime profile / GUI 设置共用同一解析器；不得在 `run_runtime_mode` 里再加分支状态，应由一个小的 codec-selection 组件输出最终 codec。
2. **HEVC 码率预算**：当前 0.08 bpp 与 codec 无关，切换 HEVC 只会在同码率下提升画质，**不会省带宽**。若目标是降码率，`build_low_latency_encoder_profile()` 需要按 codec 区分 target/floor（HEVC 系数需实测，常见经验为同画质 H.264 的 60–75%，此处仅为待验证假设），并保持 `stream_video_max_width=0` 原生尺寸基线。
3. 能力探测拆分：H.264 与 HEVC 分别探测；HEVC 硬件可用性需要一次真实 `avcodec_open2` 探测（或首次打开失败即记录），不能只看编码器名。
4. 回落策略：HEVC 硬件不可用或打开失败时，**回落 H.264 同一硬件后端**，而不是 `libx265`。`libx265` 仅作为显式诊断选项（CPU 代价高，`thread_count=1` 下桌面原生分辨率 30 FPS 能否维持未验证）。
5. 关键帧恢复：`libx265` 未设置 `forced-idr`（FFmpeg 默认 0），`pict_type=I` 可能产出非 IDR 的 I 帧：若编码为 CRA 会被 `payload_contains_hevc_keyframe()` 判为关键帧但随机接入语义需验证；若编码为普通 TRAIL I 片则不会被识别——与已修复的 NVENC 问题同类（见 `docs/testing/desktop-latency-quality-investigation-20260915.md`）。`hevc_amf` 同样没有显式 IDR 选项；QSV 的 `forced_idr` 只在 `zero_latency_tuning` 分支内设置。需对每个 HEVC 后端核验“请求刷新 → 立即 IDR（NAL 19/20）→ 全新解码器单帧可出图”。
6. 关键帧判定建议收紧：恢复用关键帧只认 IDR（19/20）及携带 VPS/SPS/PPS 的帧；CRA/BLA（16–18、21）在无 B 帧时通常可随机接入，但应由测试证明后再接受。

### 3.2 分片/传输 Packetize & transport
- 分片头已有 codec 字节，HEVC **不需要改分片格式**；Annex B + repeat headers 同样适用。
- HEVC 关键帧仍可能很大（当前 H.264 现场关键帧 110–156 KB），分片数、pacer deadline、关键帧准入规则不变即可复用，但需测量 HEVC IDR 大小。

### 3.3 协商 Negotiation（必须）
- 在 `StreamControlMessageV1` 的 Capabilities/Hello 中加 **可选** 字段，例如 `video_decode_version` + `repeated VideoDecoderCapabilityV1{codec, profile(Main), max_width, max_height, hardware, low_latency}`。字段缺失 = 旧端 = 只保证 H.264。
- Host 选择规则：`hevc` 仅当 (Controller 声明 HEVC Main 且尺寸满足) ∧ (Host HEVC 硬件编码探测通过) ∧ (设置允许)。否则 H.264，并在诊断中记录原因（`codec_selected=h264 reason=peer_no_hevc_report` 等）。
- 时序：Host 现在可能在收到 Capabilities 前就开始编码。需要在“首帧编码前等待能力（有界超时，超时用 H.264）”与“先 H.264、收到能力后重建编码器为 HEVC 并发 IDR”之间二选一；后者对首帧延迟更友好，且 Controller 侧 `ensure_decoder()` 已支持按 codec 重建。
- 未知 codec 值：Controller 目前计为 decode failure 并持续丢帧，不是显式 `protocol_version_incompatible`。有协商后 Host 不会发送对方未声明的 codec，这一行为可保持。

### 3.4 解码 Decode
- Windows Controller：已有 HEVC 解码与 D3D11VA 硬解路径。风险在于 GPU 不支持 HEVC DXVA 配置时，FFmpeg 仍可能返回硬件配置，首帧解码失败后才回落软件；软件 HEVC 在原生分辨率下 CPU 明显高于 H.264（待测）。能力上报应基于真实硬解探测，而不是“FFmpeg 有 HEVC 解码器”。
- 会话中途硬解失败（已解出过帧后）不会回落软件——HEVC 下应补“请求 H.264 降级”或“重建软件解码器 + 请求 IDR”路径。
- Android：MediaCodec HEVC 硬解在 arm64 设备上较普遍但非全部；低延迟特性（`FEATURE_LowLatency`, API 30+）与输出缓冲深度因厂商而异，可能比 AVC 多缓存帧。必须用 `MediaCodecList` 上报，`createDecoderByType("video/hevc")` 失败不能成为首次发现手段。

## 4. 风险表 / Risk table

| # | 风险 Risk | 严重度 Sev | 可能性 Likelihood | 缓解 Mitigation |
| --- | --- | --- | --- | --- |
| R1 | Host 发送 Controller 无法（实时）解码的 HEVC → 黑屏/卡死 | High | High（若直接改默认） | 可选能力上报；缺字段即 H.264；Host 不发送未声明 codec |
| R2 | 期望“降码率”落空：预算与 codec 无关 | Medium | Certain（若不改预算） | codec 区分预算；同场景 H.264/HEVC 同口径码率-画质对比后再定系数 |
| R3 | HEVC 硬件编码不可用/探测误报（名称存在 ≠ GPU 支持；旧 NVIDIA/Intel/AMD、虚拟机、远程会话显卡） | High | Medium | 分 codec 真实打开探测；失败回落 H.264 同后端，不落 `libx265` |
| R4 | 软件 HEVC 编码（libx265）CPU 过高，帧率崩溃 | High | High（落到软件时） | 产品默认禁止自动落到 libx265；仅显式诊断开启 |
| R5 | 关键帧恢复失效：x265/AMF 强制 I 帧非 IDR，丢包后等到自然 GOP（~2 s）才恢复 | High | Medium | 每个后端设置并读回 IDR 选项；新增“请求刷新 → NAL 19/20”回归用例 |
| R6 | 延迟：HEVC 编码/解码单帧耗时、硬件管线深度可能高于 H.264 | Medium | Medium | B 帧已强制 0；按阶段计时（编码、解码、呈现）对比；超阈值自动回落 H.264 |
| R7 | 丢包鲁棒性：HEVC 帧更小但单帧丢失同样需 IDR；IDR 体积/恢复时间未知 | Medium | Medium | 测 IDR 字节与恢复时间；沿用 latest-frame/keyframe-required 语义 |
| R8 | Controller 软解回落只在首帧前有效，中途硬解失败无路可退 | Medium | Low–Medium | 解码失败计数 → 重建软件解码器 + keyframe request，或请求 Host 降级 H.264 |
| R9 | 混合版本：旧 Controller（v0.1.0+ 已能映射 codec=2）不上报能力；旧 Host 永远发 H.264 | Medium | Certain（存在旧端） | 缺字段 = H.264；双向混合版本、滚动升级、重连纳入门禁（AGENTS.md 协议规则） |
| R10 | Android MediaCodec HEVC 缺失或高延迟 | Medium | Medium | `MediaCodecList` 上报 profile/尺寸/low-latency；不满足则 H.264 |
| R11 | 专利/许可：HEVC 专利池（多个池与独立权利人）比 H.264 更分散；GPU 厂商硬件编解码、FFmpeg/x265 软件实现、分发方式各有不同许可状况 | Medium（产品决策） | Unknown | 产品层面需法务评估（本文不构成法律意见）；默认 H.264 可降低暴露面；优先使用 OS/GPU 驱动提供的硬件编解码路径，避免默认分发软件 HEVC 编码 |
| R12 | 诊断/测试缺口：零 HEVC 用例，看板“done”误导 | Medium | Certain | 增加 HEVC roundtrip、关键帧恢复、codec 切换用例；修正看板描述 |

## 5. 推荐分阶段 / Recommended rollout

1. **P0 准备（无行为变化）**：拆分 H.264/HEVC 能力探测；HEVC 编码器 IDR 选项核验；HEVC roundtrip + 强制刷新用例（NVENC/QSV/AMF 有硬件时运行，软件 x265 仅测功能）。
2. **P1 能力上报**：Controller（Windows，后续 Android）在 Capabilities 中上报可选 `video_decode` 能力；Host 只记录，不改变选择。混合版本测试验证旧端无影响。
3. **P2 可选 HEVC（默认 H.264）**：`stream_video_codec=auto|h264|hevc`，默认 `h264`；`auto/hevc` 仅在双方能力满足时生效，否则回落 H.264 并记录原因。codec 感知码率预算作为该阶段的一部分，用实测数据定系数。
4. **P3 评估默认 `auto`**：同 LAN 与异地矩阵都满足“同画质码率显著下降、端到端延迟不劣化、关键帧恢复 ≤ H.264”后，才考虑默认 `auto`；H.264 永久保留为兜底。
5. Android：spike 封存期间不改；恢复后先接 P1 能力上报，再允许 HEVC。

## 6. 验证矩阵 / Validation matrix（同 LAN 优先）

每项均需 `--stream-require-capture` 真实采集证据，记录 codec、后端、分辨率、30 s FPS、编码/解码阶段耗时、Host CPU/GPU、实际码率、IDR 字节、恢复时间。

| 场景 Case | Host 编码 | Controller 解码 | 期望 Expected |
| --- | --- | --- | --- |
| V1 基线 | H.264 NVENC/QSV/AMF | D3D11VA | 现有基线数值 |
| V2 同 LAN HEVC | HEVC 同后端 | D3D11VA | 码率/画质对比、延迟不劣化 |
| V3 HEVC 软解 | HEVC 硬件 | 强制软件 | CPU/FPS 可接受或上报为不支持 |
| V4 无 HEVC 硬编 Host | 探测失败 | — | 回落 H.264 同后端，不落 libx265 |
| V5 丢包/恢复 | HEVC | D3D11VA | 强制刷新立即 IDR，恢复时间 ≤ H.264 |
| V6 分辨率变化/重连/采集代次变化 | HEVC | D3D11VA | 编码器重建后首帧为 IDR，无冻结 |
| V7 混合版本 新Host↔旧Controller | auto | 旧版（无上报） | 选择 H.264 |
| V8 混合版本 旧Host↔新Controller | 旧版 H.264 | 新版 | 正常 H.264 |
| V9 中途 codec 切换 | H.264→HEVC | D3D11VA | 解码器重建、无黑屏 |
| V10 Android（spike 恢复后） | auto | MediaCodec | 按上报选择；不支持时 H.264 |
| V11 异地 DHT/ICE | HEVC | D3D11VA | 与 V2 同口径，含 TURN/弱网 |

## 7. 未决问题 / Open questions

- 目标是“同画质降码率”还是“同码率提画质”？决定 R2 的预算方案。
- 目标硬件群体中 HEVC 硬件编码覆盖率（尤其 Intel 核显与 AMD 旧卡）未知，需要 M02-T02 兼容矩阵数据。
- 产品是否接受默认分发软件 HEVC 编码（x265）——许可与 CPU 两方面都需要产品决定。
