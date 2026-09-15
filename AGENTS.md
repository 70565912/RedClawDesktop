# RedClawDesktop Agent Guide

本文件是本仓库的 AI agent 规则入口。子目录下的 `AGENTS.md` 会在对应目录内补充更具体的规则；遇到冲突时，以更深层目录规则为准，但不得违反本文件的全局安全和流程约束。

## AGENTS.md Structure

```text
AGENTS.md                  # 仓库全局约束
.github/AGENTS.md          # GitHub workflow、prompt、instruction 约束
src/AGENTS.md              # C++/CMake 模块实现约束
tests/AGENTS.md            # CTest、单元测试、集成测试约束
docs/AGENTS.md             # 文档树、看板、交接规则
scripts/AGENTS.md          # PowerShell 运维脚本规则
installer/AGENTS.md        # Windows installer/WiX 规则
clients/AGENTS.md          # 移动端和便携客户端规则
```

## Project Snapshot

RedClawDesktop 是面向开发者远程接入自己开发机的 Windows-first 桌面产品。当前主线是 C++20 + CMake + VS2022 + vcpkg，辅以可选 Qt6 Widgets GUI、Windows service、PowerShell 运维脚本、WiX 安装器，以及 Android-first 便携客户端规划。

当前最高优先级（2026-05-10 起）：

- 先打通双机桌面画面传输显示闭环：Host 采集桌面画面，经过编码/传输，Controller 解码并显示可见画面。
- 验收入口必须是桌面 UI：本机机器码、对方机器码、等待/连接按钮、控制端画面播放区域。用户流程不得要求输入 IP 地址。
- 必须使用在线 rendezvous 信令和 ICE/STUN/TURN 内网穿透。直连 IP/TCP、文件信令、本地双进程 smoke 只作为诊断手段。
- 必须保留真实编解码链路。preview-only/RGB 包不能作为 P0 通过证据。
- 模块边界服务于这条纵向链路。允许为了端到端闭环同时修改 `net`、`capture`、`render`、`session`、`protocol`、`service`、`helper`、`ui` 等相关代码。
- 暂停追求过细的模块级测试矩阵。新增测试只覆盖当前闭环的关键风险和回归点。
- 区分“链路 smoke”和“真实采集 gate”：`--stream-smoke` 可用于验证传输/渲染，`--stream-require-capture` 必须用于确认 Host 真实桌面帧已进入链路。

核心原则：

- Direct-first: 优先 P2P 直连，中心化服务只作为可选路径或正式在线 rendezvous 信令路径。
- Security-by-default: 默认最小暴露面，高风险能力必须显式开启并 fail closed。
- Docs-as-runtime: 文档是执行协议，任务状态以运行时文档为准。
- E2E-before-polish: 先拿到真实可运行链路，再补细节、清理和硬化。
- Performance-and-simplicity: 性能与简洁设计是全项目持续约束，不是 hardening 阶段才处理的收尾项。

## Performance and Simplicity Guardrails

- 禁止用重复分支、重复状态、重复协议层、长期并存的临时兼容路径或大段复制粘贴来“堆出”功能。新增实现前先确认现有抽象能否复用；替代路径达到兼容门槛后，应删除或明确限定旧路径，而不是无限叠加。
- 热路径（采集、编码、分片、传输、重组、解码、渲染、输入）必须优先减少整帧/整包复制、临时分配、锁竞争、轮询唤醒、同步磁盘 I/O 和无界队列。队列必须有容量上限，并为实时画面明确 latest-frame、drop/backpressure 语义。
- “硬件加速”“共享内存”“零拷贝”等结论必须以真实数据流为准。GPU -> CPU readback -> GPU upload 不能称为零拷贝；仅有硬件后端名称也不能替代阶段耗时、CPU/GPU、内存带宽和帧率证据。
- 性能优化不得默认牺牲画面尺寸、真实采集或编解码链路来掩盖瓶颈。继续保持 `stream_video_max_width=0` 的原生尺寸基线；弱网或诊断降档必须显式开启。
- 设计应保持最小充分复杂度。避免为假设中的未来需求提前引入多层抽象、缓存、线程或框架；优先选择可测量、可删除、边界清晰的最小实现。
- 不得继续扩大超大入口函数或集中式可变状态。涉及 `run_runtime_mode`、`launch_gui_shell` 等编排入口的新功能，应优先提取有明确所有权、生命周期和 typed contract 的组件，避免新增捕获大量引用的长 lambda 和跨线程共享布尔/计数器。
- 性能相关变更必须先记录基线，再给出同口径的前后对比；至少覆盖相关的 FPS/延迟/CPU/内存/复制或队列指标之一。无法运行实测时，必须明确标记为静态审计结论或待验证假设，不得声称性能已改善。
- 代码评审同时检查功能正确性、热路径成本和简洁性。若新增复杂度不能由当前验收目标、可复现性能数据或安全边界证明，应缩减设计。

## Mandatory Startup Flow

1. 先读 `README.md`，它是唯一强制入口。
2. 按任务需要读取：
   - `docs/runtime/PROJECT_STATE.md`
   - `docs/runtime/MODULE_KANBAN.md`
   - `docs/runtime/MODULE_PLAYBOOKS/<module>.md`
   - `docs/runtime/AGENT_EXECUTION_PROTOCOL.md`
   - `docs/setup/coding-conventions.md`
3. 优先执行 `docs/runtime/PROJECT_STATE.md` 中的双机桌面画面闭环优先队列。只有当用户明确给出 `module: Mxx`，才按模块任务选择。
4. 不要回滚用户已有改动。开始编辑前检查工作区状态，编辑范围必须和任务相关。

## Remote Agent Result Contract

涉及跨机器 Agent 协作，尤其涉及远端权限、进程、解压验包、部署或运行诊断时，必须先完整读取并使用 [remote-agent-result-contract](.agents/skills/remote-agent-result-contract/SKILL.md)。若技能未出现在选择器中，直接读取此仓库文件，不以未自动加载为由跳过。

控制端只约定目标、已授权范围和验收结果，不获取远端权限/进程等原始信息、不指定远端命令或接管执行。远端 Agent 自行选择方法，在本机判断权限、落实必要安全检查并执行，仅返回脱敏结果；缺少授权时在本机请求合法批准，不通过委托绕过限制。一个阶段受阻不得阻塞其他已授权且无依赖的阶段。

## Work Selection

- 当前默认不是“按模块扫 todo”，而是按双机桌面画面闭环优先队列推进。
- 首选能让链路更接近可见画面的任务：启动路径、信令/连接、捕获、编码、传输、解码、渲染、联调脚本、日志证据。
- 暂缓与闭环无直接关系的细分任务，例如大规模断言迁移、格式化基线、移动端、剪贴板/文件传输、长跑稳定性、过细的模块单测补全。
- 需要跨模块接口变化时，在 `docs/logs/DEVLOG.md` 记录接口变化摘要。
- 完成任务时更新：
  - `docs/runtime/MODULE_KANBAN.md` 的任务状态和模块 Updated 日期。
  - `docs/logs/DEVLOG.md` 的阶段级摘要，避免逐文件流水账。
- 需要交接时，按 `docs/agent-handoff/AGENT_HANDOFF_PROTOCOL.md` 追加 handoff block。

## Tech Stack Baseline

- Language: C++20。
- Compiler/build: MSVC via Visual Studio 2022, CMake >= 3.20。
- Dependency manager: vcpkg manifest mode, `vcpkg.json`, target triplet `x64-windows`。
- Main native deps: OpenSSL, boost-json, gtest, spdlog, fmt, libqrencode, ffmpeg, libdatachannel with `srtp`。
- GUI: optional Qt6 Widgets. CLI/runtime fallback must remain useful when Qt is unavailable.
- Tests: CTest targets under `tests/`, with unit and integration targets split by naming.
- Scripts: PowerShell with PSScriptAnalyzer settings in `PSScriptAnalyzerSettings.psd1`.
- Installer: WiX v4 scaffold under `installer/`.
- Portable client: Android-first, recommended Flutter. mac/web mini access are planning-only unless explicitly re-scoped.

## Build and Test Guardrails

Treat CMake configure/build as serialized in this workspace. Use one entrypoint at a time.

**Agent 强制规则：凡是需要编译主程序，必须使用 `.\build.ps1`，禁止直接调用裸 `cmake --build` 命令（测试目标除外）。**
直接调用 `cmake --build` 会跳过发布步骤，导致 `release\<Configuration>\` 目录里的二进制不是最新产物。

**标准本地开发工作流（构建 + 自动发布到 `release\<Configuration>\`）：**

```powershell
.\build.ps1                          # Debug 构建并发布到 release\Debug\
.\build.ps1 -Configuration Release   # Release 构建并发布到 release\Release\
.\build.ps1 -SkipConfigure           # 跳过 configure，重新编译并发布
.\build.ps1 -NoPublish               # 仅编译，不发布
```

`build.ps1` 等价于以下序列：
1. `cmake --preset vs2022-x64-local`（configure，除非 `-SkipConfigure`）
2. `cmake --build --preset debug-local`（或 `release-local`）
3. `publish.ps1 -Configuration <Config>`（复制产物 + windeployqt 到 `release\<Config>\`）

**例外：只运行测试目标时，允许直接调用 cmake 底层命令。**

```powershell
cmake --build --preset debug-local --target <focused_test_target>
```

如需手动补发布（例如仅运行了 cmake 底层命令后）：

```powershell
.\publish.ps1                        # 发布 Debug 到 release\Debug\
.\publish.ps1 -Configuration Release # 发布 Release 到 release\Release\
```

Run focused tests only when they directly cover touched code or the desktop-stream path:

```powershell
cmake --build --preset debug-local --target <focused_test_target>
ctest --test-dir build/vs2022-x64 -C Debug --output-on-failure -R "<focused_test_name>"
```

If using VS Code tasks, prefer the guarded wrapper:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-cmake-guarded.ps1 --build --preset debug-local --parallel 8
```

If configure/build reports active-operation or lock errors such as `another operation is already in progress`, `C1041`, `LNK1104`, or `MSB6003`, stop launching new builds and follow `docs/setup/cmake-tools-api-failure-troubleshooting.md`.

Do not block the desktop-stream milestone on exhaustive module test sweeps. Use full CTest only before a release-style checkpoint or when broad shared behavior changed.

For stream work, record both normal smoke evidence and strict capture evidence when possible. A run that only transmits synthetic preview frames is useful diagnostics, but it is not a real desktop proof.

## Local Files and Generated Artifacts

- Do not commit local machine paths or secrets.
- `CMakeUserPresets.json` is local-only. Use `CMakeUserPresets.example.json` as the template.
- `build/`, `vcpkg_installed/`, logs, temp files, `.env*`, and runtime signaling artifacts are generated/local.
- `runtime-signaling/` contains local signaling exchange artifacts unless a task explicitly says otherwise.
- Never print passphrases, tokens, private keys, fingerprints that are meant to remain private, or raw secret-bearing runtime configs in logs.

## Security and Runtime Rules

- Remote control and privileged input paths must fail closed on missing capability, lost lifecycle channel, invalid token, expired consent, or secure-desktop ambiguity.
- Keep Windows-specific code behind `_WIN32` and isolate platform code where practical.
- Prefer typed contracts and parser/validator functions over ad hoc string manipulation for protocol, config, and runtime profile data.
- 所有跨版本交换的线上协议和持久化协议，包括 DHT 信令、会话、媒体、控制、Agent 与 Debug Bridge，必须具备显式版本和能力协商，并在所有仍受支持的已发布版本之间保持前后兼容。新旧端必须按共同能力集互通，禁止把“相同 Git commit、相同程序版本或相同二进制”设为连接条件或产品验收前提。
- 协议演进应优先使用可选字段和增量能力；接收端必须容忍未知可选字段。确需不兼容语义时，必须引入新 schema、迁移路径和明确的 `protocol_version_incompatible` 诊断，不得以解析失败、无远端记录或无限等待代替版本错误，也不得静默降级安全门禁。
- 每次协议变更必须验证最新版本与仍受支持旧版本的双向混合版本连接、滚动升级和重连；同版本双端测试只能作为基线，不能替代兼容性门禁。
- Sealed-file signaling uses encrypted short text; external channels exchange ciphertext only. Shared passphrases must be treated as secrets.
- Structured logs and diagnostics must redact sensitive values.

## GitHub and Network Operations

- Any GitHub-connected operation such as `git fetch`, `git pull`, `git push`, `gh`, or repository sync must be announced to the user first and only run after explicit confirmation.
- Do not create, rewrite, or force-push branches unless the user asked for that exact operation.
- CI workflow edits must preserve the Windows/unit-test baseline unless the task explicitly changes the release gate.

## Completion Response Contract

When finishing a task, report:

- Completed task IDs, if any.
- Files changed.
- Checks run and result: at minimum build result; when possible include local loopback, dual-process, or dual-machine evidence such as logs, frame counters, screenshots, or observed controller display.
- Remaining risks or skipped validation.
- Next recommended task ID when it is useful.
