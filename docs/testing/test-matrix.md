# 测试矩阵与自动化流程

Updated: 2026-09-20

本页是测试选层、自动化入口和证据判定的唯一总表。验收是“相关本地用例 + 必要的异地最小闭环”，不是每个功能依次在三层重测。功能、权限、性能是测试维度，不是要求更多机器的理由。历史日志保留事实；旧手册中的重复验收要求按本页分层执行。

自动化序列和版本发布验收只收录能无人值守完成的用例。需要人工按键、登录/授权操作、准备特殊桌面或拓扑、判断画面的用例完全移出自动序列，归开发者自行选择的人工评估；不记为自动化 skip/待补验，也不作为发布阻塞项。不删除这些测试工具或产品安全检查，不把“未评估”说成已验证。

## 1. 三种执行位置

| 层 | 验证什么 | 自动化方式 / 人工边界 |
| --- | --- | --- |
| A 本机独立 | 协议解析/版本/安全门禁、状态机、输入映射、文件哈希与取消、ConPTY、UI 布局和可无人值守的 OS 集成 | CTest + 自动创建/清理的隔离 fixture；需要人工环境或真实键盘的用例不注册到自动序列。 |
| B 本机双端 | 真正的 Host/Controller 串联、共同能力协商、混合版本、断线恢复、跨通道操作门禁、完整媒体链路 | 复用双 GUI runner；自动启动自己的进程、采样、判定和清理。可以使用在线 DHT，但不代表已验证异地 NAT。 |
| C 异地最小闭环 | 真实 public-DHT 可达性、异地 ICE/NAT 路径、配置的 TURN、代表性的通道往返 | 仅已有无人值守场景才可进入自动序列。当前需要人工配合的联调由开发者自行决定，不属于发布验收。 |

安全桌面拒绝、未知能力和旧版本互通仍必须验证。不得删除 `LLKHF_INJECTED` 过滤、绕过焦点/授权、降画质或以合成帧使自动化“通过”。UAC 可见/可控和开机登录属于明确启用的服务特性，不是普通桌面输入验收的前置条件。

## 2. 本机自动入口

在已配置的构建目录中运行（示例为 Ninja；VS 用户替换为实际目录）：

```powershell
# 默认 5 个快速核心套件；只构建测试目标，不发布或重启主程序。
.\scripts\service\run-local-validation.ps1 -BuildDirectory build/ninja-x64

# 按影响范围组合，重叠套件只执行一次。
.\scripts\service\run-local-validation.ps1 -Area input,workspace -BuildDirectory build/ninja-x64

# 只列计划，无构建、无测试、无网络。
.\scripts\service\run-local-validation.ps1 -Area agent,ui -List
```

入口复用现有 CTests，不增加第二套功能测试实现；当前目录中有多个构建树时必须明确选一个。目标经 CTest 发现核对，缺少目标或零用例不得通过。人工项在 CTest 注册处排除，不先执行再跳过；直接 CTest 和本入口使用同一边界。构建使用现有串行 guard；执行按套件串行、有超时，不自动重试失败、不提权、不启动远端、不启用真实 Provider 登录/原生输入。不要与其他构建/CTest 操作重叠。

| `-Area` | 自动覆盖范围 | 不包含的证明 |
| --- | --- | --- |
| `smoke`（默认） | Control 协议、输入会话、操作门禁、终端协议、Agent broker | 不是所有功能/真实键盘/异地验收 |
| `input` | 普通输入策略、适配器、会话、Control 协议、paste guard | 真实物理键盘 Hook 与目标应用消费 |
| `workspace` | 文件/终端协议、文件接收/worker/runtime、哈希/冲突/取消、剪贴板数据准备/copy store、ConPTY 会话 | 原生 clipboard/前台焦点与实际应用 Ctrl+V 在人工评估清单，不阻塞自动化/发布 |
| `agent` | 协议、peer、broker、coordination、Provider 解析与生命周期 | 真实账号登录/执行归开发者可选评估，不要求每次 Provider smoke |
| `media` | 采集恢复状态、拥塞反馈、分片重组、真实编解码 roundtrip、确定性媒体仿真 | 仿真不等于真实 GPU 采集或 WAN |
| `connection` | DHT 故障存储、连接协商、transport recovery | 不等于公网 DHT/NAT 可达性 |
| `ui` | 自动窗口布局/展开、Agent/Debug 本地控制、终端 pipe/布局 | 可能显示自有窗口，但不要求人工操作；原生键盘、fixed WebView 手工配置和日志回放用例已排除 |

精确目标清单以 [runner](../../scripts/service/run-local-validation.ps1) 的 `-List` 为准。新用例加入已有相关套件，新增独立目标时再更新分组；用 [入口自测](../../scripts/service/test-local-validation.ps1) 检查目标注册、去重及失败/跳过分类。`-SkipBuild` 只用于刚构建过的同一产物或环境复验，报告保留 `build_skipped`，不声称源码新鲜度。

每次生成独立的 `build/reports/local-validation-*/result.json`、`ctest.xml`、`cases/*.xml` 和构建/测试日志。记录提交、dirty 状态、EXE 哈希、CTest 中的用例过滤条件、实际用例和退出码。旧式非 GoogleTest 程序只报告套件结果。结果/退出码仅有 `passed/0`、`failed/1`、`blocked/3`（缺自动化环境/产物）；自动集合中意外 skip 属于用例或运行环境错误，不进入“等待人工补验”状态。人工项不计入自动总数。旧的 `partial` 回执保留为历史，不代表当前流程。

## 3. 用例归属与最小完成条件

| 用例 | 主验收层与现有入口 | 完成条件 / 何时才补异地 |
| --- | --- | --- |
| 协议/能力、安全拒绝、失效租约、旧 epoch | A：相关 `input/workspace/agent/connection` CTests | 正反例确定性通过；新旧支持版本能力取交集，不要求同 commit。 |
| 文件/文件夹、空目录、Unicode、冲突/占用/取消/断开 | A：`workspace`；B：双端真实传输抽样 | 接收内容/哈希、完成/取消和临时数据清理符合约定。C 只需新增 channel 的代表性小文件往返，不重做全部文件组合。 |
| 剪贴板格式、目标变更、一次粘贴、copy store | A：数据准备/paste guard/copy store | 自动检查载荷、门禁与文件结果。实际应用粘贴、前台焦点和专用 window station 属人工评估，不要求补验。 |
| 普通鼠标/键盘 | A：`input` + 无需前台授权的 UI 回归 | 自动检查协议/策略/会话；真实物理键盘和前台应用响应归开发者选测，不是发布门禁。 |
| 终端交互、Unicode、resize、重连保留 Shell、清理 | A：`workspace` 与终端 pipe/布局 | ConPTY、协议、清理、布局自动断言；手工配置 WebView/原生按键及异地终端交互单列可选评估。 |
| Agent 解析、边框、展开、队列、审批重放 | A：`agent`/`ui`；B：fixture Provider 通道往返 | 自动化使用隔离 Provider fixture；实际账号或新审批属于人工评估，不进发布条件。 |
| 真实采集、GPU、DPI、多显示器 | B：可无人值守的真实媒体场景；硬件专用工具单独保留 | 特殊桌面、GPU/驱动覆盖和人工看图归开发者选测。不能将仿真说成真实采集或 WAN 验证。 |
| 支持版本双向互通、滚动升级、重连 | B：双 GUI 的 `HostRuntimeExe` / `ControllerRuntimeExe` / `HostRestartCount` | 所有支持版本双向和共同能力门禁保留，组合在本机跑。改到传输/协商时 C 补代表性混合版本路径，不复制全部矩阵。 |
| 安装包/升级/回滚/发起 Agent 退出 | A/B：[独立升级](runtime-directory-upgrade.md)、`test-runtime-directory-upgrade.ps1` | 在隔离运行目录验证身份/损坏/占用/回滚和幂等；正式远端部署是单独授权操作，不作为功能测试默认动作。 |
| DHT 可达性、NAT、TURN、真实 WAN 重连 | C：[可选联调手册](cross-lan-dual-machine-integration-playbook.md) | 需要人工环境/配合的项目不纳入自动化或发布要求，由开发者决定是否评估。 |
| 并发输出/弱网/长期性能 | A：仿真有界队列；B：同场景实测；C：仅拓扑特定风险 | 沿用 [性能基线](product-performance-baseline-v1.md)，不把每轮功能检查变成长跑或无节制压力测试。 |

## 4. 执行流程与人工干预预算

1. 按改动路径选 A 的分组；纯文档只做链接/一致性检查，脚本只做自身回归和受影响入口。没有改动主程序不重建发布包。主程序构建仍只能用 `build.ps1`；Debug/Release/full CTest 留到发布检查点或共享行为广泛变化。
2. A 失败先定位本机首个断点；仅跨组件行为变化才补 B。B 的构建与发布做一次，同一 Debug 产物复用 `run-local-dual-gui-integration-test.ps1 -SkipBuild -AgentFixtureProvider`，避免引入真实账号/审批；版本组合交换两端路径即可。该 runner 默认在线 DHT，可用 `-SignalTransport file` 做离线诊断，但不能冒充 C。若某场景仍需人工介入，则不放入自动序列或发布验收。
3. C 只有在无人值守场景已具备时才运行自动部分。需要人工配合时，不自动申请对端协作或阻塞发布；由开发者自行决定是否执行手册。一场会话复用结果，不为每个断言另建任务/审批/重启；不关闭 Job 清理安全机制。
4. 自动保存结果并只上报失败/跳过清单与证据路径。失败保留首次记录，定向复验失败项；无关已通过用例不陪跑。证据复用必须匹配构建产物、相关代码/配置/依赖和所声称环境；不得复用被修改路径的旧通过记录。
5. 自动序列结束即给出自动用例的结果，不追加人工待办或索取“已重现”。人工评估由开发者主动选择；未选择的项目不列成版本验收缺口。产品权限/安全门禁保持不变，不能为了无人值守伪造物理输入或代批授权。

一次可归属的应用结果足以证明“应用已消费”：目标日志/机器可读回执、目标内容读取、Controller 中清晰可见的计数/文本变化任选其一，并绑定场景/时间。仅 `SendInput` 返回成功、发送/接收计数或 ACK 不够；已经看见目标响应，不再强制追加一次远端 Agent 独立回读。失败或证据矛盾时才拉取双端分段计数诊断。

## 5. 开发者可选人工评估（不入自动序列，不入发布验收）

CTest 已将混合套件中的以下 12 个原生/诊断用例通过命令参数排除，自动部分保留：clipboard 的原生前台/独立 window station（4）、真实 Provider readiness（1）、UI 前台输入回调和指定日志回放（2）、终端原生按键/fixed WebView（2）、采集硬件恢复/GPU 光标对照（2）、单帧 roundtrip 内的专用硬件 decode（1，其他软件编解码 roundtrip 仍自动执行）。

三个真实桌面探测工具 `redclaw_capture_dda_min_capture_poc_tests`、`redclaw_capture_windows_capture_abstraction_skeleton_tests`、`redclaw_capture_stability_smoke_tests` 保留构建目标，但不注册 CTest，也不加入自动测试聚合目标。开发者需要时可直接构建/运行；混合 GoogleTest 程序可直接用 `--gtest_filter=<明确用例>` 调用。不要通过自动流程逐个提示人操作。

实际应用粘贴、物理键盘、UAC、Provider 登录、人工图像/体验判断、临时异地拓扑和真实部署均由开发者自行评估。日后某场景真正实现安全无人值守后，再作为新的自动用例加入；当前不以“将来会自动化”为由放入发布条件。

## P0 Desktop Stream Local Simulation Matrix

媒体/连接状态机变化时运行此已有确定性入口；已由本次 `-Area media,connection` 覆盖的套件不再重复运行。它不是每次请求异地诊断前的强制全量前置：

```powershell
.\scripts\service\run-local-desktop-stream-simulation-matrix.ps1
```

The runner builds only the focused test targets and writes a versioned JSON result plus build/CTest logs under `build/reports/local-stream-simulation-<timestamp>/`. The fixed seed is recorded in the result so a failure can be replayed exactly.

| Scenario | Actual components exercised | Required invariant |
|----------|-----------------------------|--------------------|
| Static source | retained IDR, transport estimator, congestion controller, control-heartbeat clock | Zero new media is not loss; stale receiver pressure cannot reduce the rate; control remains live. |
| Remote log after reconnect | control epoch guard, bounded remote-log request/chunk/complete messages | A retained request is restamped for the replacement epoch; old-epoch replay is rejected and the new snapshot completes. |
| Viewport replay after reconnect | control epoch guard and sticky viewport request | The same viewport is restamped and resent three times in the replacement epoch, so one transient accepted-but-lost send cannot leave Host without the current display target. |
| Forced active source | v3 fragment serialization/parsing, transport feedback, estimator, reassembler | Changed frames complete continuously and drain in-flight metadata. |
| Seeded random fragment loss | real fragment boundaries, dependency gate, latest complete IDR recovery | Incomplete/dependent P frames never render; a later complete IDR restores output. |
| Weak network | production token bucket and congestion controller | Traffic stays bounded, pressure backs off, and the pacing floor is respected. |
| Capacity step down/up | fresh queue evidence and stable-window probing | A sudden drop reduces pacing; sustained recovery probes upward without an immediate oscillation. |
| Open channels without first media | production stream-health classifier and typed Debug status | After both required channels open, zero first-media progress fails at 10 seconds as capture/encode/transmit/receive/playback startup stall; a real delivered frame clears the gate. |
| Revision/age isolation | payload-free sent metadata ring and transport estimator | Old feedback may retire only old in-flight metadata and cannot alter the current rate revision. |
| DHT loss/delay | encrypted `DhtRendezvousClient` records over a deterministic fault store | Offer, answer, and acknowledgement converge after dropped publish/fetch attempts and delay. |
| DHT out-of-order record | DHT revision/generation contract | A late old record cannot roll back the visible revision or ICE generation. |
| DHT/ICE negotiation state | production connection-negotiation state machine | Persistent offers, duplicate/reordered delivery, late candidates, failed transports, and bounded reconnect converge safely. |
| Failed DHT generation isolation | production recovery/generation policy | A Controller repair cannot re-adopt the failed Host generation; its fresh request replaces an answered but unconnected Host attempt and waits for a strictly newer offer. |
| Real codec roundtrip | FFmpeg encoder/decoder across three resolutions | Low-latency encoded payloads decode, and resolution changes reset dependency state without stale output. |
| Host post-session request takeover | persistent Host offer policy and negotiation coordinator | A healthy session rejects a competing request; after disconnect/reset, a fresh Controller request replaces standby and advances to a newer Host generation. |

The local dual-GUI runner consumes the same `health` value from the runtime rates line. It stops immediately on a stable `*_startup_stalled` classification instead of waiting for the outer run timeout and records `failure_classification` plus `first_media_deadline_ms` in `result.json`.

The runner's default 300-second outer limit is deliberately longer than one 90-second ICE attempt so the public-DHT path can exercise one complete failed-generation repair. It still exits as soon as the full media/UI gate is stable.

This matrix does not emulate public-DHT reachability, NAT mapping, ICE reachability, GPU drivers or WAN scheduling. GPU/driver behavior belongs to native local coverage; real network topology belongs to C. Record each endpoint's version/hash independently: same-SHA is not an interoperability requirement. Local automation owns state-machine regression; cross-LAN runs own external-environment evidence.

## 历史 KPI 参考（不是已部署的自动门禁）

| Metric | Target | Proposed regression observation |
|--------|--------|-----------------|
| Session setup time | <= 10s typical | PR: warn if > 8s; Nightly: fail if > 12s |
| End-to-end input latency | <= 80ms (same-region healthy link) | Nightly: fail if p95 > 100ms |
| Reconnect after transient drop | <= 5s | Nightly: fail if > 8s |
| Memory growth (8-hour run) | No unbounded growth | Nightly: fail if RSS delta > 50 MB |
| Frame capture latency (M04) | <= 16ms per frame (60fps) | Nightly: warn if p95 > 20ms |
| NAT direct connection rate | >= 95% full-cone, >= 80% port-restricted | Nightly: alert on regression |

These are historical initial targets, not evidence that PR/nightly jobs implement these checks. Apply the effective performance contract below; do not create manual cross-LAN repetitions to satisfy this reference table.

For GUI/input/Agent concurrent performance, the effective 2026-09-11 contract is
[Product Performance Baseline v1](product-performance-baseline-v1.md). It keeps the
100 ms input, 250 ms running-heartbeat/ACK and 90% paired-FPS thresholds for a
Release-optimized, paced representative workload, adds a 15 FPS absolute floor, and
separates that product decision from the Debug unpaced 1 MiB isolation stress. Existing
Debug failures remain recorded. `DEV-FUNC-1` now passes on real-path identity and correctness;
the current hardware-calibrated performance observations are within their alert lines but do
not participate in feature-development pass/fail. `PB-REL-1` remains unverified for the release
candidate decision. Correctness failures remain blocking in every phase.

## CI 与发布

- PR 使用已存在的 [Windows unit workflow](github-actions-validation.md)，本入口不修改或替代该基线。开发按影响面选测，不默认全量。
- 当前未部署的 nightly/自动异地计划不能写成已存在的门禁；长跑和额外 NAT 类型按对应任务单独执行。
- 发布检查点只要求可自动完成的构建、CTest 自动集合、包完整性/提取启动及支持版本自动互通/重连。人工用例、人工异地最小闭环、真实键盘/粘贴/UAC/账号或视觉确认一律不作为发布条件。自动通过仅证明所执行范围，不能宣传未执行的物理环境覆盖；服务 MSI 等未发布能力仍不属于便携包。
