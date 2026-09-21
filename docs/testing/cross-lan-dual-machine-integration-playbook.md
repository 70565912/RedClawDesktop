# 跨 LAN 双机 GUI 联调手册

本手册验证正式产品路径：Host 和 Controller 通过机器码、public-DHT rendezvous 与 ICE 建立连接，Controller 显示 Host 的真实桌面，并保持 Control、Media、Agent 三通道工作。CLI、文件信令和合成预览只用于诊断，不能替代本手册的通过证据。

本手册是开发者可选的现场评估工具，不是版本发布检查表。凡需要人工登录、授权、按键、看图或准备异地环境的步骤，都不进入自动化序列或发布验收要求；开发者自行决定是否执行。下文“通过”只描述所选场景的结果，不代表必须补做该场景才能发布。

## 0. 范围：一次最小闭环，不是全部用例异地重跑

按 [测试矩阵](test-matrix.md) 选层。本机已完成的协议、文件边界、剪贴板格式、终端功能、UI 布局、审批状态机、升级故障和混合版本组合不在这里重复。真实网络/对端环境相关变化可参考本手册，不能因准备发布而强制申请一轮人工联调。

| 用例 | 自动采样/判定 | 人工边界 |
| --- | --- | --- |
| C-01 建连与真实媒体 | public-DHT → ICE 路径与通道；真实采集/发送/接收/解码/呈现增量，无合成/新增失败 | 首次配置机器码、必要授权和一次视觉确认 |
| C-02 代表性输入 | 自有目标上的一次点击与一次 QA 键消息，结果由目标回执或播放画面读出 | 不要求每轮人工按键；真实物理键盘 Hook 在本机另测 |
| C-03 Agent 往返（启用时） | 一次已授权的有界任务，到终态并收到结果 | 真实登录或新的权限请求由用户处理，不代批 |
| C-04 可选通道（新增/修改时） | 读取实际共同能力，传一个小文件核对哈希、终端一次有界输出；同通道 clipboard 不再遍历格式 | 不用升级/重启远端来证明已有本地功能 |
| C-05 恢复/relay（相关变更或对应发布覆盖） | 一次受控恢复；TURN 在有配置的拓扑单独记录 | 物理断网、路由变更仅按已授权范围执行 |

批量执行前一次性准备场景。复用已有 supervisor、Debug Control 状态/证据导出和 Agent 结果回执；没有统一自动编排的部分要标明，不能声称全自动已经实现。不要每个断言再建 Agent 任务、再要用户回“已重现”、再让两端重建。测试目标由独立场景拥有，不能依附即将结束的 Agent turn；不得因此禁用 Job 清理。遇到失败只对该链路取证。

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
- 本机监听端口、路由器外部端口和对端收到的公网 ICE 候选端口要分别核对。例如 Host 内部 UDP 55000 被映射为外部 UDP 55001 时，Controller 应能收到并探测 Host 公网 UDP 55001；Controller 自己仍可监听 UDP 55000。包含 X00-T19 修复的映射端会额外发布该标准候选，无需接收端改成本地监听 55001。
- 需要手工路由器规则时，仅映射 **UDP 55000 → 当前电脑 UDP 55000**；如 GUI 配置了其他 ICE 端口，则映射该实际端口。
- DHT 监听端口无需手工映射。
- 基线优先使用具有默认路由的物理 Ethernet/Wi-Fi。使用 VPN、TUN/TAP 或代理出口时应单独记录，不与物理出口基线混算。
- 若直连受 CGNAT 或对称 NAT 阻断，配置双方可访问的 TURN 服务再重测。

当前 GUI 提供 `Auto (system route)` 和具体网卡地址。Auto 由 Windows 路由决定，可能跟随 TUN/VPN 虚拟网卡；普通 HTTP/HTTPS 系统代理不承载当前 public-DHT 或 ICE UDP。正式异地直连基线使用显式物理网卡，代理/TUN 作为独立实验，TURN 作为难穿透网络的 relay 验证。

## 3. 构建与发布

仅缺少匹配产物或相关代码/依赖发生变化的一端需要构建。已经验证的发布包直接复用，独立记录身份；不默认在两端各重建一次。需要构建主程序时，在该电脑串行执行：

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

也可双击仓库根目录的 `Start-RedClaw-Debug-Host.cmd`。它只启动 Host 角色，并选择本机具有 IPv4 默认路由的物理出口；不要在同一台电脑上同时启动 Host 和 Controller。

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

### 三通道与可选 workspace

- Control、Media、Agent 均保持打开；
- Agent 面板能读取能力与项目目录，授权任务可返回完整终态；
- 一次有界输出期间媒体与控制持续工作；洪泛、队列边界和所有审批排列在本机自动化中验证。
- 新增/修改 workspace 通道时运行 C-04，记录实际协商能力；从源代码或同一提交推断版本不能替代协商证据。

### 输入

在 Host 明确授权后，从 Controller 的播放窗口启动控制，仅对场景自有目标验证一个键事件和一个鼠标事件。可用现有 QA 消息入口自动化，保留正常授权/焦点/几何/会话门禁；这不声称验证物理键盘 Hook。目标机器可读结果或 Controller 可见的目标计数/文本变化足以证明应用消费，不要求再让对端 Agent 独立回读一次。仅有注入计数/ACK 不算应用响应。

失焦、租约到期、通道断开或安全桌面状态必须暂停输入并释放按键；完整状态组合归本机回归，有相关改动才追加代表性检查。结束时暂停控制，避免向用户工作窗口遗留输入。

### 重连

选中 C-05 时才在首次连接通过后做一次受控断网/恢复，不因 UI/文件逻辑改动重复此项。要求 Host 继续等待，Controller 能建立新 generation，恢复后的第一组完整关键帧重新推进呈现。重连后远程输入保持暂停，直到用户再次授权。支持版本的完整双向/滚动升级组合在本机双端执行；网络/协商变化补代表性异地混合路径。

## 7. 证据导出

场景结束导出一次；失败才追加对应断点快照。双方各自执行或由已授权的 endpoint Agent 本机完成并返回结果摘要：

```powershell
.\scripts\service\invoke-cross-lan-debug-control.ps1 `
  -Action export_evidence -Role host -EvidenceTimeoutMs 60000 -Json
```

Controller 将 `-Role` 改为 `controller`。保存并核对：

- 提交 SHA、EXE 或发布 ZIP SHA256；
- 角色、运行 ID、有效网络出口、ICE UDP 端口和映射结果；
- DHT、ICE 与三通道状态；
- Host 捕获/编码/发送计数；
- Controller 接收/重组/解码/呈现计数；
- 输入授权和可归属的应用响应；故障诊断再按需关联发送/接收/注入/ACK；
- 所选 C 用例的通过/失败/跳过、证据位置；现有程序保持运行则不要求退出码，不为取证关闭它；
- 一份播放截图或等价的应用结果，不为同一结论重复要求截图、人工确认和 Agent 回读。

证据目录属于本机生成数据，不提交到 Git。

## 8. 失败分类

| 现象 | 首先检查 |
| --- | --- |
| DHT 未就绪 | 物理出口、bootstrap 解析、监听错误和发布产物新鲜度 |
| ICE 端口绑定失败 | 55000 是否被占用、Windows 排除端口和 GUI 配置 |
| 有信令但 ICE 失败 | 双方候选、STUN 可达性、NAT 类型、防火墙、TURN 配置 |
| UPnP 显示成功，但对端始终收不到 ICE 回包 | 比较 `mapped_external_port` 与实际公布的公网候选端口；`send_target_match` 只说明探测符合收到的候选，不证明候选符合路由器映射 |
| 通道打开但无画面 | Host 真实采集/编码/发送与 Controller 接收/重组/解码/呈现的首个断点 |
| 画面存在但输入失败 | Host 授权、焦点、几何版本、租约和 ACK 分类 |
| Agent 不可用 | Host 的 Provider 登录状态、注册项目和显式 Agent 授权 |

每次失败保留原始结果并修复首个确定断点。不要以增加等待时间、降低画质或跳过真实采集来制造通过结果。

端口探测与桌面连接分别记账：2026-09-20 的 Controller 诊断确认收到的公网候选为 UDP 55000，而对同一公网地址 UDP 55001 的独立认证 ICE 探测一次发送即收到一次成功回包，消息完整性校验通过。这证明当次临时源端口到映射端口可达，不证明原程序 UDP 55000 的候选对已连接，也不证明媒体链路通过。原始地址、凭据、进程和探测记录仅保存在本机报告中；正常发布和验收不要求读取进程内存。
