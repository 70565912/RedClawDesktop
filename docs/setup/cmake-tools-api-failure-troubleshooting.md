# CMake Tools API 构建失败排障（Windows / VS2022）

本文用于定位和规避以下常见现象：
- CMake Tools API 构建偶发返回失败（例如 result code 为 -1 或无有效错误输出）。
- 同一时段命令行任务构建成功，但 CMake Tools API 报失败。

## 结论（本仓库已复现）
该类问题在本仓库的主因不是 CMakePreset 配置错误，而是 CMake Tools 扩展调度与并发构建冲突。

边界说明：
- 若日志出现 `config.h(9): error C2001` 且失败点在 `ffmpeg:x64-windows`，通常不是本文描述的并发锁冲突。
- 该类问题请转到：`docs/setup/vcpkg-ffmpeg-locale-troubleshooting.md`。

## 2026-06-30 事故复盘：focused build 长时间卡住
这次问题必须作为后续排障优先匹配项处理。表象是 focused target：

```powershell
.\build.ps1 -Configuration Debug -SkipConfigure -Target redclaw_service_dht_rendezvous_tests -NoPublish -Parallel 1
```

两次长时间不结束（一次约 6 小时，一次超过 3 小时）。后续监控复盘确认它不是 DHT runtime 问题，也不是 vcpkg 下载/安装问题，而是构建入口和 Ninja/MSVC 规则共同暴露的问题。

已确认的坑：
- `build-desktop.ps1 -> run-cmake-guarded.ps1` 旧调用方式用 `-CMakeArgs @(...)` 透传 CMake argv，跨 `powershell.exe -File` 时会丢掉第一个 token 之后的参数。
- 典型错误日志是 wrapper command 退化为 `cmake.exe --fresh` 或 `cmake.exe --build`，而不是完整的 `cmake.exe --fresh --preset ninja-x64-local` / `cmake.exe --build --preset debug-local ...`。
- 旧 wrapper 没有 stdout/stderr 落盘、没有总超时、没有静默超时、没有进程快照；一旦 Ninja 卡住，只能看到“挂了很久”，不能看到最后卡在哪条 edge。
- 05:10 卡住的时间线指向 `dht_rendezvous_libtorrent.cpp.obj -> redclaw_service.lib`：obj 和 compile PDB 写出，但 `.ninja_log`、`.ninja_deps`、service lib、test exe 未更新。
- 旧 Ninja 规则使用 MSVC `/Zi /Fd... /FS`，存在共享 compile PDB / `mspdbsrv.exe` handle stall 风险。

已落地的修复：
- 主构建入口仍是仓库根目录 `.\build.ps1`。
- wrapper argv 改为 `-CMakeArgsBase64` 传递完整 CMake 参数列表；`run-cmake-guarded.ps1` 解码后执行。
- wrapper 会拒绝裸 `--fresh`、裸 `--build`、`--fresh` 无 `--preset` 等不完整 CMake 调用，避免 false success。
- wrapper 现在写 `build/reports/cmake-guard/*.out.log` / `*.err.log`，并有 command timeout、quiet-output timeout、heartbeat、进程快照和清理兜底。
- 根 CMake 将 MSVC Debug/RelWithDebInfo debug info format 切到 embedded (`/Z7`)；必须 fresh configure 后才会体现在 `build\ninja-x64\CMakeFiles\impl-Debug.ninja`。

验证证据：
- 监控命令：

```powershell
.\build.ps1 -Configuration Debug -FreshConfigure -Target redclaw_service_dht_rendezvous_tests -NoPublish -Parallel 1 -ConfigureTimeoutSeconds 900 -BuildTimeoutSeconds 900 -NoOutputTimeoutSeconds 180
```

- 结果：configure 完整执行 `cmake --fresh --preset ninja-x64-local`，focused build 完成 `[19/19]`。
- 关键产物更新到 `2026-06-30 10:36`：`dht_rendezvous_libtorrent.cpp.obj`、`redclaw_service.lib`、`redclaw_service_dht_rendezvous_tests.exe`、`.ninja_log`。
- 生成规则里 DHT service object 的 `FLAGS` 含 `-Z7`。

补充复盘（11:22 `redclaw_desktop` 发布构建）：
- 普通权限/沙箱下运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Configuration Debug -SkipConfigure -Target redclaw_desktop -Parallel 1 -BuildTimeoutSeconds 1800 -NoOutputTimeoutSeconds 600
```

- watchdog 看到 `cmake.exe -> ninja.exe` 运行 600 秒无输出，但没有 `cl.exe`，`.ninja_log` 和 `redclaw_desktop.exe` 均未更新，`.ninja_lock` 残留且普通权限删除被拒绝。
- 同一条构建在提升权限并清理 stale `.ninja_lock` 后 34 秒完成 `[20/20]`，随后发布到 `release\Debug\`。
- 因此该现象应优先判定为本地权限/lock/子进程观测问题，而不是源码编译超慢或 DHT 代码卡住。
- 若再次出现“只有 `cmake+ninja`、无 `cl/link`、日志空、`.ninja_log` 不推进”的状态，先确认无构建进程残留，再清理 stale `.ninja_lock`；在 Codex/受限 shell 中可能需要提升权限执行构建或清理。

后续遇到同类现象时先看：
- `build/reports/build-monitor-*/build.out.log`
- `build/reports/build-monitor-*/build.err.log`
- `build/reports/cmake-guard/*.out.log`
- `build/reports/cmake-guard/*.err.log`
- wrapper 输出中的 `command:` 行是否保留完整 CMake argv。

已捕获到的关键信号（来自 VS Code CMake 输出通道日志）：
- another operation is already in progress（重配置请求被占用）
- error C1041（PDB 文件锁）
- LNK1104（ILK 文件锁）
- MSB6003（编译文件被其他进程占用）

一次会话中的统计样本：
- another operation is already in progress: 2
- error C1041: 5
- LNK1104: 2
- MSB6003: 6

## 根因机制
1. 同一工作区存在并行构建来源
- 例如同时触发：
  - CMake Tools API 构建
  - VS Code task 中的 cmake --build --preset ...
  - 其他触发编译的命令

2. 编辑 CMakeLists 后触发自动重配置
- CMake Tools 在检测到 CMakeLists 变更时会尝试重配置。
- 若此时已有构建/测试在执行，会出现 another operation is already in progress。

3. MSBuild/CL 并发写入冲突
- 多个构建流程同时写同一输出目录，容易触发 PDB/ILK/OBJ 锁冲突，表现为 C1041/LNK1104/MSB6003。

4. Ninja/MSVC 共享编译 PDB 卡住
- 2026-06-30 的 DHT focused target 长时间卡住不是 configure/vcpkg，也不是误选全量测试目标。
- 取证时间线显示 `.ninja_lock` 已创建，`dht_rendezvous_libtorrent.cpp.obj` 和 `redclaw_service.pdb` 已写出，但 `.ninja_log`、`.ninja_deps`、`redclaw_service.lib` 和测试 exe 没有更新。
- 当 Ninja 规则使用 MSVC `/Zi /Fd... /FS` 时，编译期共享 PDB 写入和 PDB server/handle 行为可能让 Ninja 卡在单个 compile edge 周围，表现为没有 `cl.exe` 残留但 `ninja.exe` 仍不退出。
- 仓库根 CMake 已切换 MSVC Debug/RelWithDebInfo 的 debug info format 为 embedded (`/Z7`)；需要重新 configure 后才会体现在生成的 Ninja 规则中。

5. PowerShell wrapper 参数转发丢参
- 2026-06-30 的 monitored retry 发现 `build-desktop.ps1` 通过 `-CMakeArgs @("--fresh", "--preset", "...")` 调用 `run-cmake-guarded.ps1` 时，PowerShell 只可靠传入第一个 token。
- 典型症状：wrapper 日志显示 command 退化为 `cmake.exe --fresh` 或 `cmake.exe --build`，随后 CMake 输出 usage/errors，但上层脚本可能继续执行。
- 当前修复：`build-desktop.ps1` 使用 `-CMakeArgsBase64` 传递完整 CMake argv；`run-cmake-guarded.ps1` 解码后执行，并拒绝 bare `--fresh` / bare `--build` 等不完整调用。

## 规避策略（推荐）
1. 单一入口原则
- 一次只使用一种构建入口：
  - 要么使用 CMake Tools API
  - 要么使用 tasks.json 的 cmake --build
- 不要两者并发。

2. 构建前确认无占用
- 确认没有正在运行的构建/测试任务。
- 若刚执行过 CTest 或其他长命令，等待其结束后再触发下一次构建。

3. 出现 busy/锁冲突时的恢复流程
- 停止当前构建任务。
- 执行一次显式 configure，再 build：

```powershell
.\build.ps1 -FreshConfigure -NoPublish
.\build.ps1 -SkipConfigure -NoPublish
.\build.ps1 -SkipConfigure -Target redclaw_tests_unit -NoPublish
```

- 如仍异常，可清理后重试：

```powershell
.\build.ps1 -FreshConfigure -NoPublish -Parallel 1
.\build.ps1 -SkipConfigure -NoPublish -Parallel 1
.\build.ps1 -SkipConfigure -Target redclaw_tests_unit -NoPublish -Parallel 1
```

4. 工作流建议（Agent/多人协作）
- 在同一时间窗口内，避免一个流程跑 CMake Tools API，另一个流程跑 Task 构建。
- 先串行完成 build，再串行跑 CTest，降低输出目录锁争用概率。

## 团队执行清单（建议设为强约束）
每次构建前快速确认：
1. 当前只选择了一种构建入口（API 或 task，不混用）。
2. 没有其他构建/测试任务正在运行。
3. 如果刚改过 CMakeLists，等待当前操作结束后再触发下一次构建。
4. 出现 busy 或锁冲突后，先恢复（configure -> build）再继续测试。

Agent 快速判定片段（与 README 保持一致）：
1. 需要主程序 configure/build 时使用仓库根目录 `.\build.ps1`。
2. 若返回 exit code `42`，判定为并发冲突，不要继续发起新构建。
3. 若返回 exit code `124` 或 `125`，先检查 `build/reports/cmake-guard/*.out.log`、`*.err.log` 和 watchdog 输出里的进程快照，再决定是否重试。
4. 若日志里的 wrapper command 只剩 `--fresh`、`--build`，或缺少 `--preset <name>`，先修 `build-desktop.ps1` / `run-cmake-guarded.ps1` 参数转发，不要把它当源码编译失败。
5. 执行恢复：
  - `.\build.ps1 -FreshConfigure -NoPublish`
  - `.\build.ps1 -SkipConfigure -NoPublish`
  - `.\build.ps1 -SkipConfigure -Target redclaw_tests_unit -NoPublish`
6. 构建成功后再继续 CTest/后续任务。

建议统一默认构建入口：
- 日常本地开发优先使用 tasks.json 的 preset 构建；
- CMake Tools API 仅在需要扩展能力（如目标级选择）时使用。

当前仓库默认防并发脚本：
- VS Code 构建任务默认使用 `--parallel 8`（构建图并行，仍受 guard 锁保护）。
  - 输出可执行恢复步骤（configure -> build），
  - 返回 exit code `42`，用于快速识别“并发构建冲突”而非源码编译失败。

## 证据位置
- 若机器资源紧张或出现编译阶段不稳定，可将 `.vscode/tasks.json` 中 `--parallel 8` 调低（例如 4 或 2）。

可检索关键词：
- another operation is already in progress
- error C1041
- LNK1104
- MSB6003
