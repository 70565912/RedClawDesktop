# 网络出口选择与异地联线概率分析

Updated: 2026-09-14

## 结论

在当前实现中，**显式绑定物理 Ethernet/Wi-Fi 比普通 Windows 系统 HTTP 代理更有利于建立异地双机连接**。原因不是物理网卡天然能够穿透所有 NAT，而是 public-DHT、ICE/STUN 和 ICE UDP 端口目前直接使用网络套接字；普通 HTTP/HTTPS 代理不承载这些 UDP 流量。代码中读取 Windows 默认代理的 WinHTTP 路径只服务于中心化 `rendezvous` HTTP 信令，不能替代 DHT、ICE 打洞或媒体 DataChannel。

“系统代理”需要再分两种情况：

- 仅提供 HTTP/HTTPS/PAC 的系统代理：只能影响支持代理的 HTTP 请求，对当前 public-DHT 和 ICE UDP 的联线概率没有直接帮助。
- 提供 TUN/VPN 虚拟网卡并接管系统路由、且允许 UDP 的代理软件：当前程序会把它视为系统默认路由或一个可选网卡。它有时能绕开受限网络，也可能引入额外 NAT、禁止 UDP 或让 UPnP 面向错误网关，效果取决于该隧道服务，不能归类为普通系统代理。

如果目标是提高各种 NAT 和防火墙环境下的总体可达率，最有效的产品方案仍是：直连候选优先，并配置可用的 TURN 中继作为失败后的 ICE relay 候选。网络出口选择提高可控性和诊断能力，但不能代替 TURN。

## 本次范围

本报告记录当前行为、代码边界、历史证据和后续修正要求。本次不修改运行时代码，不改变正在运行的 Host，也不把历史日志文件数解释为独立试验成功率。

## 当前代码行为

### System default

GUI 首项为 `Auto (system route)`，值为空。运行时在这种情况下：

- ICE 固定 UDP 端口在未指定地址的套接字上预约，libdatachannel 不设置 `bindAddress`；
- DHT 监听 `0.0.0.0`，启用 IPv6 时同时监听 `[::]`；
- Windows 路由表决定实际出站路径；多个物理、虚拟或隧道接口都可能产生候选。

相关实现见 `src/ui/gui_shell.cpp:283-369`、`src/main.cpp:2518-2539`、`src/main.cpp:5361-5370`、`src/net/src/net_module.cpp:675-680` 和 `src/service/src/dht_rendezvous_libtorrent.cpp:165-183`。

### Specific adapter

GUI 枚举处于 Up/Running 状态的接口和非回环 IPv4 地址，优先排列 Ethernet/Wi-Fi，其次排列 Virtual/PPP/SLIP。用户选择某个地址后，同一个 `network_bind_address` 被传给：

- ICE UDP 端口预约；
- libdatachannel ICE/STUN/TURN 候选收集；
- DHT IPv4 监听及所选接口的可用 IPv6；
- ICE UDP 的 UPnP 映射尝试。

因此显式选择物理网卡可以排除 Hyper-V、VPN 和其他虚拟接口候选，并让 DHT、ICE 和 UPnP 的出口一致。代价是被排除的接口不能作为备用路径；若所选地址失效，启动会明确失败。

### System proxy

当前没有独立的 `system_proxy` 网络出口模式。`WinHttpOpen` 使用 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`，但调用链只在 `signal_transport=rendezvous` 的 HTTP 信令实现中启用，见 `src/main.cpp:775-805` 和 `src/main.cpp:2495-2504`。public-DHT、STUN、TURN 和 DataChannel 没有接入 WinHTTP，也没有实现 SOCKS5 UDP、HTTP CONNECT-UDP 或 MASQUE。

所以当前 GUI 中“Auto 可能跟随代理或隧道适配器”的提示只描述 Windows 路由可能被 TUN/VPN 改写，并不表示普通系统代理能够承载 ICE。

## 历史证据

原始日志和机器信息保存在被 Git 忽略的本地证据目录；这里只记录脱敏结论。

| 时间与场景 | 出口 | 观察结果 | 能证明什么 |
| --- | --- | --- | --- |
| 2026-09-14，同一 Host 启动对照 | System default → 显式物理网卡 | 两次均 DHT reachable、ICE UDP 55000、UPnP `no_igd`；本地 host 候选从 3 个降为 1 个，srflx 均为 2 个；对端尚未发布记录 | 显式物理网卡确实排除了额外接口候选，但该窗口没有对端，不能证明异地成功率 |
| 2026-08-22，Controller 异地运行 | System default | `connected=true`、媒体通道打开，接收/解码/呈现均达到 3347，解码和呈现失败为 0；UPnP 仍为 `no_igd` | System default 可以完成异地直连和真实媒体；UPnP 失败并不必然阻断 ICE |
| 2026-09-11，Host 异地运行 | 显式物理网卡 | public-DHT 和 direct-NAT 路径成功；真实采集/编码/发送达到 61459/21913/21902，`synthetic=0`，相关失败为 0，所需通道曾全部打开 | 显式物理网卡也能完成真实异地媒体链路 |
| 2026-09-11，换向 Controller 重试 | 显式物理网卡 | 多次收到远端描述，但未进入 `connected=true`，UPnP 为 `no_igd` | 绑定物理网卡不是 NAT 穿透保证；信令成功也不等于 ICE 成功 |

对 89 个 `cross-lan-gui-*` 证据目录的粗略扫描得到 65 个含可识别网络绑定配置的 runtime 日志：显式地址 39 个、System default 26 个；两类各有 12 个日志包含 `connected=true`。这些目录跨越多个版本、角色和重复重试，同一次试验可能生成多个文件，因此只能证明两类路径都有成功与失败，不能计算或比较成功率。没有历史日志使用独立 `system_proxy` 传输，因为代码尚不存在这种模式。

## 联线概率评估

| 选择 | 对当前 public-DHT / ICE UDP 的作用 | 可达率判断 | 主要限制 |
| --- | --- | --- | --- |
| 显式物理 Ethernet/Wi-Fi | DHT、ICE、STUN/TURN 与 UPnP 绑定到同一接口 | 当前三者中最适合做异地直连基线；出口明确，在多网卡机器上通常更稳定、更容易诊断 | 仍受 CGNAT、对称 NAT、UDP 防火墙和运营商策略影响；选错或断开的接口没有备用路径 |
| System default | 由 Windows 路由决定，可收集多个接口的候选 | 日常使用通常最方便，也可能因更多候选获得备用路径；在虚拟网卡、VPN 或多默认路由环境中结果不够可预测 | 可能生成无效虚拟候选，UPnP 所在网关与实际出口可能不一致，证据难归因 |
| 普通系统 HTTP/HTTPS 代理 | 只影响中心化 HTTP rendezvous | 对当前 DHT/ICE 直连没有提升 | 不能传输任意 DHT/ICE UDP，也不能承载现有媒体 DataChannel |
| TUN/VPN 虚拟网卡 | 作为系统路由或特定适配器承载 IP 流量 | 服务允许 UDP 并提供合适出口时可能提高受限网络可达性；也可能降低直连概率 | 额外 NAT、禁 UDP、较高延迟、UPnP 不可用及服务端策略 |
| TURN relay | 作为 ICE relay 候选 | 对难穿透 NAT 的提升最可靠，应作为正式异地覆盖的回退路径 | 需要可访问的 TURN 服务、带宽和运维成本，媒体不再是端到端直达路径 |

因此，若只在“物理网卡”和“普通系统代理”之间选择，当前版本应选物理网卡。若比较“显式物理网卡”和“System default”，没有固定胜者：单一路由、无虚拟接口时两者常等价；多网卡或代理隧道环境中，物理网卡更可控，System default 保留更多候选。正式测试使用显式物理网卡，日常产品默认可继续使用 System default，并允许用户覆盖。

## 后续配置修正

Host 和 Controller 的启动路径都应保存和应用以下选择：

1. `System default`：不指定绑定地址，遵循系统路由。
2. `Specific adapter`：选择物理、虚拟或隧道网卡；界面显示接口类型、稳定接口标识和当前地址。
3. `System proxy`：明确显示作用范围。仅中心化 HTTP 信令支持时必须标注“signaling only”；在 DHT/ICE 代理承载实现完成前，不能把它显示为全连接出口或静默退回 System default。

配置应保存“模式 + 稳定接口标识”，运行时解析当前地址；不能只永久保存 DHCP 地址。诊断至少输出 `network_exit_mode`、接口标识、有效本地地址、代理来源与类型、DHT 监听路径、ICE 候选接口、UPnP 网关、候选对和最终 transport path。更改模式应在下一次 ICE generation 生效，并在界面提示需要重连。

TURN 服务器及其 relay 策略继续作为 ICE 配置，不能伪装成系统代理模式。

## 实施里程碑

- M1 — 配置模型：新增有类型的出口模式、稳定接口标识、代理作用范围和有效配置诊断；CLI、profile 与 GUI 使用同一验证器。
- M2 — GUI：Host 和 Controller 均提供 System default、Specific adapter、System proxy 三种选择；不支持的协议组合在启动前明确说明。
- M3 — 运行时：DHT、ICE、UPnP 和 HTTP rendezvous 分别按照声明的作用范围应用出口；禁止把只影响信令的代理报告为媒体出口。
- M4 — 验证：覆盖配置保存、接口地址变化、接口消失、多网卡、TUN/VPN、纯 HTTP 代理、UDP 被阻断、TURN relay 和角色换向。

## 依赖

- Qt 网络接口枚举与 Windows 稳定接口标识映射；
- libdatachannel/libjuice 对绑定地址、TURN transport 和候选诊断的能力；
- WinHTTP 系统代理解析，用于中心化 HTTP rendezvous；
- 若要让系统代理承载完整连接，需要另行选择并实现明确的 UDP 代理或代理可达 TURN 方案。

## 风险

- “系统代理”语义含糊会让用户误以为媒体和 DHT 已经走代理；
- 只保存 IP 地址会在 DHCP 更新后绑定错误或无法启动；
- 强制单一网卡会减少 ICE 的备用候选；
- 代理凭据、PAC 结果、内网地址和候选信息可能进入日志，必须继续脱敏；
- 对只支持 TCP/TLS 的网络，如果没有可用 TURN，增加界面选项也不会提高 ICE 可达率。

## 验收标准

- Host 与 Controller 可以分别选择并保存三种模式，CLI/profile/GUI 的优先级一致；
- System default 不设置显式绑定，Specific adapter 的 DHT、ICE 与 UPnP 均使用所选接口；
- System proxy 的界面、状态和日志准确说明实际覆盖的协议，未支持的 DHT/ICE 代理路径明确失败或标记不可用；
- DHCP 地址变化后按接口标识重新解析，接口消失时启动失败且给出可操作提示；
- 双机矩阵至少覆盖物理直连、System default、多网卡、TUN/VPN、纯 HTTP 代理和 TURN relay；
- 每轮保存候选类型、候选对、最终 transport path、DHT 结果、UPnP 结果、三通道及真实画面计数；
- 不以增加超时、降低画质或把信令成功当作 ICE 成功完成验收。
