<div align="center">

# RedClawDesktop

**面向开发者的 Windows P2P 远程桌面与远端 Agent 工作台**

[English](README.en.md) · [下载 v0.1.3](https://github.com/70565912/RedClawDesktop/releases/tag/v0.1.3) · [构建文档](docs/README.md) · [问题反馈](https://github.com/70565912/RedClawDesktop/issues)

[![Release](https://img.shields.io/badge/release-v0.1.3-blue)](https://github.com/70565912/RedClawDesktop/releases/tag/v0.1.3)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4?logo=windows)](https://github.com/70565912/RedClawDesktop/releases/tag/v0.1.3)
[![License](https://img.shields.io/badge/license-Apache--2.0-green)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](CMakeLists.txt)
[![Qt 6](https://img.shields.io/badge/Qt-6-41CD52?logo=qt)](https://www.qt.io/)

</div>

RedClawDesktop 让开发者通过机器码连接自己的 Windows 开发机，在同一个界面中查看真实桌面、发送经过授权的键鼠输入，并与开发机上的 AI coding Agent 交互。连接采用在线 DHT rendezvous 与 ICE/STUN/TURN 协商，优先建立端到端 P2P 直连。

> `v0.1.3` 是 Windows x64 Developer Preview，合并采集恢复、Host 光标与文件/剪贴板/终端工作台。尚未提供签名安装器，也不宣称覆盖所有异地 NAT/TURN 组合。详见 [发行说明](docs/releases/v0.1.3.md)。

![RedClawDesktop Controller 正在显示真实视频链路与 Agent 面板](docs/assets/redclaw-controller-desktop-stream.png)

## 为什么使用 RedClawDesktop

- **开发场景优先**：桌面画面、键鼠控制和 Agent 输出在一个工作台中协同。
- **机器码连接**：正常产品流程无需填写对端 IP 地址。
- **P2P 优先**：DHT 用于在线 rendezvous，ICE/STUN/TURN 用于连接协商和 NAT 穿透。
- **真实媒体链路**：Host 采集并编码桌面，Controller 解码并呈现；诊断预览不能替代真实链路验收。
- **明确授权边界**：远程输入、无人值守能力和 Agent 任务均受显式授权、租约和项目范围约束。

## 当前能力

| 能力 | v0.1.3 状态 |
| --- | --- |
| Windows Host / Controller 图形界面 | 可用 |
| 机器码与 public-DHT 在线 rendezvous | 可用 |
| ICE/STUN 直连、可配置 TURN | 可用，网络兼容性仍在扩展验证 |
| WGC / Desktop Duplication 真实桌面采集 | 可用 |
| H.264/HEVC 编解码与 D3D11 呈现 | 可用，具体后端取决于硬件与驱动 |
| 授权键盘和鼠标控制 | 可用，安全桌面等场景会拒绝或暂停 |
| Control / Media / Agent 三通道 | 可用 |
| Codex / Cursor Provider 桥接 | 可用，需在 Host 上单独配置和授权 |
| 双向文件/文件夹传输 | 可用，含 SHA256 校验、冲突处理、取消与逐文件结果 |
| 按需剪贴板传送 | 可用，远程画布 Ctrl+V 触发，不持续同步；粘贴受焦点和输入门禁约束 |
| 内嵌 PowerShell 终端 | 可用，ConPTY + 随包 WebView2/xterm.js，同一桌面会话重连保留 Shell |
| 独立重启与完整目录升级 | 可用，当前用户权限、启动失败回滚，不依赖发起 Agent/终端存活 |
| Windows 签名 MSI、移动端 | 后续版本 |

## 快速开始

1. 从 [v0.1.3 Release](https://github.com/70565912/RedClawDesktop/releases/tag/v0.1.3) 下载 `RedClawDesktop-windows-x64-v0.1.3.zip` 和 `SHA256SUMS.txt`。
2. 校验 ZIP：

   ```powershell
   Get-FileHash .\RedClawDesktop-windows-x64-v0.1.3.zip -Algorithm SHA256
   ```

3. 解压到一个全新目录，运行 `redclaw_desktop.exe`。仍受支持的 Host 和 Controller 版本通过共同能力互通；连接不要求相同版本或相同二进制。
4. **当前开发候选**（已发布 v0.1.3 尚无此能力，需升级包含本功能的候选）：在受控电脑输入**本机连接密码**并点击 Save（无需重复确认），保存后选择 **Host** 等待连接；在控制电脑选择 **Controller**，输入 Host 的机器码和**对方连接密码**。密码正确后自动连接，错误不会开放画面或工作台。
5. 密码使用当前 Windows 用户的 DPAPI 加密保存。Client 只在验证成功后按机器码记住密码，可修改或忘记。不支持密码验证的旧端必须升级，不能回退到无密码连接。详见[连接密码说明](docs/architecture/connection-password-v1.md)。
6. 画面出现后，按界面提示显式开启远程输入。Agent 功能需要 Host 端配置允许的项目和 Provider。

在双方均支持连接密码的前提下，文件、剪贴板和终端按双方共同能力启用；旧端不支持的新功能保持不可用，不要求两端同时升级。传输期间暂停新的桌面/终端输入及 Agent 操作，画面和已有输出继续；取消或完成后恢复符合其他授权条件的操作。

[本地工作台调用接口](docs/architecture/workspace-control-v1.md)让脚本复用当前 P2P 会话调用终端、文件和剪贴板。Debug 版本默认开启当前用户管道，Release 版本通过 `--enable-workspace-control` 显式开启；不增加公网监听端口。候选包附带 `maintenance/invoke-workspace-control.ps1`，与当前运行实例分别交付。

便携 ZIP 不写入安装目录之外的系统服务配置。首次运行可能出现 Windows SmartScreen 提示，因为 Developer Preview 尚未进行代码签名。

## ICE UDP 端口与路由器设置

- 产品默认 ICE UDP 端口为 **55000**，可在 GUI 的网络设置中修改并保存。
- 两台不同电脑可以都使用 55000。同一电脑运行两个实例时应使用不同端口；项目测试脚本使用 Host 55000、Controller 55001。
- 开启 UPnP 后，RedClawDesktop 尝试映射实际 ICE UDP 端口。如果路由器分配了不同的外部端口，程序将该实际外部地址和端口作为额外的标准 ICE 候选告知对端，本机仍监听原配置端口。UPnP 失败不会阻止 STUN、TURN 或普通打洞，但会在诊断状态中显示。
- 手工映射时选择 **UDP**，内部端口和外部端口均填写该设备配置的 ICE UDP 端口。
- DHT 监听端口只承担 rendezvous，无需手工端口映射。

不同 NAT、CGNAT、防火墙和运营商策略会影响直连结果。无法直连时可配置 TURN；当前版本尚未完成所有异地网络组合的覆盖测试。

当前版本支持 System default 和显式网卡绑定；普通 Windows HTTP/HTTPS 系统代理只影响支持 WinHTTP 的中心化 HTTP 信令，不承载 public-DHT 或 ICE UDP。Host 与 Controller 分别选择 System default、物理/虚拟网卡或系统代理的完整配置方案已记录在[网络出口选择与异地联线概率分析](docs/architecture/network-exit-selection-analysis.md)。

## GitHub 云端验证范围

GitHub Actions 在每次任务中使用新建的 Windows 虚拟机。本机已经安装的 vcpkg 依赖不会自动出现在云端；单元测试流水线会在 CMake 配置阶段安装自己的 Qt、FFmpeg 等依赖。相同 vcpkg 版本和依赖清单会复用云端二进制缓存，缓存尚未建立或依赖变化时仍需完成一次较长的构建。

发布 Pre-release 后，独立的包检查会直接下载 GitHub Release 中的最终 ZIP，核对 `SHA256SUMS.txt`，检查便携包必需文件，并运行包内 `redclaw_desktop.exe --help`。这项检查不重新构建程序，主要发现上传损坏、缺少 DLL 或命令入口无法启动等问题。

GitHub 托管虚拟机不作为真实桌面验收环境。自动测试与版本发布只要求无人值守用例；物理键盘、应用粘贴、UAC/账号、特殊硬件/拓扑和视觉判断由开发者自行选测，不是发布前待补项目。未执行的场景不算通过。详见 [自动化矩阵](docs/testing/test-matrix.md) 和 [GitHub Actions 验证说明](docs/testing/github-actions-validation.md)。

## 安全模型

- 缺少能力、授权、有效租约或生命周期通道时，远程输入默认拒绝。
- Agent 只能在 Host 明确注册的项目中工作。终端是独立的已连接桌面能力，按 Host 当前用户权限执行命令，不自动提权；文件浏览也受该用户的文件系统权限约束。
- 日志和诊断对路径、地址和凭据做边界控制，运行信令、密钥和本地配置不得提交到仓库。
- 调试用本地密文信令文件只保存在操作员指定目录，仓库不承担通信或信令交换。

更详细的边界见 [解决方案架构](docs/architecture/solution-architecture.md)、[普通桌面输入设计](docs/architecture/ordinary-desktop-remote-input-v1.md) 和 [远端 Agent 桥设计](docs/architecture/remote-development-agent-bridge-v1.md)。

## 从源码构建

要求：Windows 10/11 x64、Visual Studio 2022、CMake 3.20+、vcpkg，以及 Qt 6 Widgets。复制 `CMakeUserPresets.example.json` 为本机的 `CMakeUserPresets.json`，填写本机依赖路径后执行：

```powershell
.\build.ps1
.\build.ps1 -Configuration Release
```

标准入口会完成构建并将可运行文件发布到 `release\Debug\` 或 `release\Release\`。不要直接用裸 `cmake --build` 代替主程序构建，否则发布目录可能仍是旧版本。详细说明见 [开发文档索引](docs/README.md)。

## 项目状态

`v0.1.3` 沿用真实桌面和 Control / Media / Agent 基线，增加可协商的远程工作台能力。自动验收与人工开发评估分开；本机双端证据不等于所有异地网络覆盖。严格性能目标作为持续观测项；性能结果会随硬件、驱动、分辨率和网络环境变化。

下一阶段重点是扩大异地网络与 TURN 覆盖、完善无人值守安装和升级、补充代码签名，并继续降低 GUI 与 Agent 大输出下的调度开销。当前执行状态见 [PROJECT_STATE.md](docs/runtime/PROJECT_STATE.md)。

## 贡献

欢迎提交 Issue 和 Pull Request。开始前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md) 和 [开发文档索引](docs/README.md)。安全问题请按 [SECURITY.md](SECURITY.md) 的私密报告方式处理。

## AI 接手单一入口

唯一必读入口：`README.md`

AI coding Agent 应先阅读本文件，再按任务需要读取根目录 [AGENTS.md](AGENTS.md)、[项目状态](docs/runtime/PROJECT_STATE.md)、[任务看板](docs/runtime/MODULE_KANBAN.md) 和相应子目录规则。

## 许可证

RedClawDesktop 使用 [Apache License 2.0](LICENSE)。发布包中的第三方组件遵循各自许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和 `third_party/licenses/`。
