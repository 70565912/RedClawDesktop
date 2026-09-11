# 跨 LAN 双机 GUI 联调手册

本手册验证正式产品路径：Host 和 Controller 通过机器码、public-DHT rendezvous 与 ICE 建立连接，Controller 显示 Host 的真实桌面，并保持 Control、Media、Agent 三通道工作。CLI、文件信令和合成预览只用于诊断，不能替代本手册的通过证据。

## 1. 双端准备

分别记录两台电脑的提交、产品/协议版本和程序哈希：

```powershell
git rev-parse HEAD
Get-FileHash .\release\Debug\redclaw_desktop.exe -Algorithm SHA256
```

仍受支持的版本必须协商共同能力并互通，不能把相同提交或相同二进制设为产品连接条件。独立本机构建的 EXE 可能因工具链不同而产生不同哈希；使用同一正式发布包做基线时则应核对 ZIP SHA256 一致。任一端工作区有未提交运行时代码时，先完成构建和发布，不要以旧 `release\Debug` 继续测试。

## 2. 网络准备

- 默认 ICE UDP 端口为 55000。两台不同电脑可以都使用 55000。
- 开启 UPnP 时，状态中应显示对 ICE UDP 端口的映射尝试及结果。
- 需要手工路由器规则时，仅映射 **UDP 55000 → 当前电脑 UDP 55000**；如 GUI 配置了其他 ICE 端口，则映射该实际端口。
- DHT 监听端口无需手工映射。
- 基线优先使用具有默认路由的物理 Ethernet/Wi-Fi。使用 VPN、TUN/TAP 或代理出口时应单独记录，不与物理出口基线混算。
- 若直连受 CGNAT 或对称 NAT 阻断，配置双方可访问的 TURN 服务再重测。

## 3. 构建与发布

在每台电脑串行执行：

```powershell
.\build.ps1 -Configuration Debug
```

该入口会配置、构建并发布到 `release\Debug\`。随后确认准备检查为 `ready`：

```powershell
.\scripts\service\prepare-dht-remote-validation.ps1 `
  -Role both -Configuration Debug -SessionCode RC7TST01 -Json
```

若 `published_runtime_fresh` 失败，重新执行标准构建；不要通过修改时间戳绕过检查。

## 4. 启动 Host

Host 先启动并保持窗口打开：

```powershell
.\scripts\service\start-cross-lan-debug-supervisor.ps1 `
  -Role host `
  -Configuration Debug `
  -SessionCode RC7TST01 `
  -IceUdpPort 55000 `
  -AutoStart
```

若电脑存在多个物理出口，可额外传入该电脑自己的 `-NetworkBindAddress <HOST_PHYSICAL_IPV4>`。不要复制另一台电脑的地址。

查询本机状态：

```powershell
.\scripts\service\invoke-cross-lan-debug-control.ps1 `
  -Action status -Role host -Json
```

继续前应看到 DHT 已监听并进入等待/发布状态；连接、媒体和输入计数此时可以为零。

## 5. 启动 Controller

Host 已等待后，在 Controller 电脑运行：

```powershell
.\scripts\service\start-cross-lan-debug-supervisor.ps1 `
  -Role controller `
  -Configuration Debug `
  -SessionCode RC7TST01 `
  -IceUdpPort 55000 `
  -AutoStart
```

也可双击仓库根目录的 `Start-RedClaw-Debug-Controller.cmd`。启动器会选择具有 IPv4 默认路由的物理出口，并在发布产物陈旧、已有冲突进程或出口不明确时明确失败。

等待最多 300 秒。不要在一个有效 ICE generation 尚未结束时反复重启双方。

## 6. 功能验收

### 连接与画面

通过需要同时满足：

- Host 的真实 `captured`、`encoded`、`transmitted` 计数持续增加，`synthetic=0`；
- Controller 的接收、完整帧重组、GUI 解码和成功呈现计数持续增加；
- 解码失败和呈现失败没有新增；
- Controller 窗口能看到 Host 当前桌面变化，不能只凭 `connected` 或 `channel_open` 判定。

### 三通道

- Control、Media、Agent 均保持打开；
- Agent 面板能读取能力与项目目录，授权任务可返回完整终态；
- 大输出不能导致媒体或控制通道永久停止。

### 输入

在 Host 明确授权后，从 Controller 的播放窗口启动控制。验证一个键盘事件和一个鼠标事件到达目标窗口，并核对 Host 注入、目标窗口接收与 ACK。失焦、租约到期、通道断开或安全桌面状态必须暂停输入并释放按键。

### 重连

只在首次连接通过后做受控断网/恢复。要求 Host 继续等待，Controller 能建立新 generation，恢复后的第一组完整关键帧重新推进呈现。重连后远程输入保持暂停，直到用户再次授权。

## 7. 证据导出

双方分别执行：

```powershell
.\scripts\service\invoke-cross-lan-debug-control.ps1 `
  -Action export_evidence -Role host -EvidenceTimeoutMs 60000 -Json
```

Controller 将 `-Role` 改为 `controller`。保存并核对：

- 提交 SHA、EXE 或发布 ZIP SHA256；
- 角色、运行 ID、ICE UDP 端口和映射结果；
- DHT、ICE 与三通道状态；
- Host 捕获/编码/发送计数；
- Controller 接收/重组/解码/呈现计数；
- 输入授权、接收和 ACK；
- 双端退出码及 Controller 播放截图。

证据目录属于本机生成数据，不提交到 Git。

## 8. 失败分类

| 现象 | 首先检查 |
| --- | --- |
| DHT 未就绪 | 物理出口、bootstrap 解析、监听错误和发布产物新鲜度 |
| ICE 端口绑定失败 | 55000 是否被占用、Windows 排除端口和 GUI 配置 |
| 有信令但 ICE 失败 | 双方候选、STUN 可达性、NAT 类型、防火墙、TURN 配置 |
| 通道打开但无画面 | Host 真实采集/编码/发送与 Controller 接收/重组/解码/呈现的首个断点 |
| 画面存在但输入失败 | Host 授权、焦点、几何版本、租约和 ACK 分类 |
| Agent 不可用 | Host 的 Provider 登录状态、注册项目和显式 Agent 授权 |

每次失败保留原始结果并修复首个确定断点。不要以增加等待时间、降低画质或跳过真实采集来制造通过结果。
