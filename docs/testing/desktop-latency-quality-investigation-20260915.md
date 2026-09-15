# 画面迟缓、输入响应与静止模糊：诊断交接

Updated: 2026-09-15

关联任务：`M08-T03`、`M05-T03`、`X00-T14`。本文是可公开的脱敏结论与后续验收协议，不包含连接码、机器身份、密钥、运行配置或完整日志。原始证据只留在各端本机被 Git 忽略的诊断目录。

## 当前结论与边界

| 现象 | 已确认的证据 | 尚不能得出的结论 |
| --- | --- | --- |
| 早期键鼠看似无响应，后来缩略图可见操作但主画面迟缓 | 历史同一 Host 区间 input received/injected 推进，rejected=0、queue=0；存在画面反馈落后的用户观察 | 空 state_sync ACK 不算实际输入，SendInput 返回成功不证明目标应用消费；原始输入故障未闭环 |
| 运行一段时间后画面严重变慢 | 历史媒体 queue **估计值**约 616–624 ms，适配到 1 FPS、pacing 400 kbps，出现帧预算拒绝；同区间发送和接收都稀疏 | 不能把估计值称为已经测得的真实网络积压；in-flight/buffered 可为 0，零丢包也不能排除时钟或调度因素 |
| 对端更新候选并重启后恢复 | 对端本机 Agent 报告约 30 s 内 encode/transmit 各 +615，约 20.51 FPS，queue 0–2 ms、pacing 2854 kbps，帧预算拒绝为 0；用户确认操作和整体流畅度恢复 | 更新和重启同时发生，未隔离代码变化与状态重置，短暂恢复不是根因修复证据 |
| 整体流畅但静止文字仍模糊 | 用户确认仅光标闪烁时仍模糊；历史源尺寸 1920×1080，编码目标 1454×818（约 57.4% 源像素数） | 缩小可能损失细节，但压缩预算、编码参数、呈现缩放各自贡献还未分离；纹理对齐尺寸不等于有效编码尺寸 |
| Agent 一直提示同步中 | 本机日志反复出现 `[request_rejected] remote agent is not authorized`；旧代码把无事件序号的请求拒绝记成任务 paused，并五秒重试 | 不是同步成功，也不是已经出现了等待操作员点击的审批；这与视频队列是不同问题 |

后续采样曾出现 GUI presented 增量为 0，但该采样不与用户观察窗口严格同源；不能用后来切回聊天的 `window_inactive` 推翻用户“播放窗口有焦点时异常”的确认。各层累计计数异步更新，应比较同实例、同区间的增量，不机械要求不同层一一相等。

## 已完成的本机同步修复

- `NormalAgentControlStateV1::observe_remote`：把 executor 的 `request_rejected` 与 Provider 任务事件区分；不覆盖任务、审批或 ACK 历史。
- 完整 capability 明确拒绝授权时，停止自动同步重试但保留待同步集合；授权恢复或新 epoch 时，未终结任务必须重新同步。拒绝期间迟到的 snapshot 不能免除恢复后的同步。
- 先验证信封，再更新 epoch；落盘失败不提交 ACK，也不让旧 replay guard 错锁新 epoch。
- `AgentConversationPanel::set_transport_state`：Offline / Unauthorized 优先于 Syncing，仍禁止未满足条件的新工作。
- 四个新增回归先复现失败再通过。最终 Debug NoPublish 构建成功；五个相关 CTest 套件通过：146 个案例通过，一个已有可选诊断绘制重放跳过。

这是本机错误归类、恢复门禁和提示修复，**不会赋予远端权限，也没有修改媒体拥塞策略**。既有被误记的历史记录没有清空或篡改，须由授权后的真实快照澄清。一般请求拒绝仍可能留下本地 queued 提示，需要显式同步确认，不能据 queued 宣称对端正在执行。

## 换机继续工作时先做什么

1. 本机按用户最新授权构建、保留旧运行目录并以 Host 模式等待；另一台电脑作为 Controller 接入。保持既有身份、项目范围和授权记录，不自动批准任务或扩大权限。
2. 先确认真实捕获、编码、发送、接收、解码和呈现均推进且 `synthetic=0`；等待状态、DHT 发布或 `channel_open` 仅是阶段证据。双方记录各自版本与能力，不要求相同 commit/EXE 才能连接。
3. **立即留下正常基线，不必等完全堵住。** 覆盖正常操作、固定文字近静止、仅光标闪动三种场景。两端分别报告同一观测窗口的增量、分辨率及是否可见更新。保留场景和显示比例，避免把两次不同窗口状态当成前后对照。
4. 正常使用中若再次变慢，先保留故障前后的有界日志和计数，再决定恢复动作。不要为了取证主动制造断网、注入输入、改时钟、提权或改配置。

跨机器执行采用 [远端结果契约](../../.agents/skills/remote-agent-result-contract/SKILL.md)：只约定目标、授权范围和验收结果。两端 Agent 自主选择本机方法并处理本机权限；对外只返回必要脱敏结果，不索要对方 PID、账号、完整命令行、权限配置或原始日志。优先现有直接 Agent 通道；拒绝授权时由执行端处理，不能借邮箱或其他通道绕过。不要重复建立调试任务或定时任务。

## 复发时如何区分瓶颈

每端独立确认样本属于同一实例和区间；实例/epoch/分辨率/速率 revision 变化时分段。原始身份留本机，交换不透明观测编号即可。

| 待区分的路径 | 最小结果证据 | 下一项验证 |
| --- | --- | --- |
| Host 采集/编码不足 | capture、encode、transmit 增量及阶段耗时，编码压力、有效尺寸/目标 FPS/bitrate | 区分真实捕获停顿、编码超时与主动适配降速，不能仅凭低发送 FPS 归因网络 |
| 媒体估计/发送准入 | 新鲜 queue/RTT queue、loss、pacing、in-flight、buffered、blocked、预算拒绝、deadline drops 与速率 revision | 把高 queue **估计**与实际本地待发队列、回调耗时分开；对齐触发降速的样本与随后恢复判断 |
| Controller 解码/呈现落后 | receive/reassemble/pipe/decode/present 增量、失败数、依赖缺口及 UI 延迟 | 若 Host 正常而呈现落后，检查帧龄、latest-frame 丢弃和 GUI 调度，不先降低编码画质 |
| 输入路径问题或仅画面反馈慢 | Controller 实际输入批次、Host received/injected/rejected/queue/last_applied_sequence 的同区间变化 | 排除空 state_sync；API 注入与应用响应分开。需要额外本机诊断权限时仅暂停相应检查，其他已授权观测继续 |

代码导航：

- 拥塞分级/恢复：`src/net/src/media_congestion_controller.cpp`，`MediaCongestionController::update`（queue ≥200 ms 严重压力、pacing 下限 400 kbps；恢复受新鲜低 queue 等条件约束）。
- 配对时间戳与估计：`src/net/src/media_transport_feedback.cpp`，`apply_feedback`、`timing_snapshot`、`format_media_transport_timing_sample`；运行日志位于 `src/main.cpp` 的周期诊断段。
- 编码目标/缩放：`src/main.cpp` 的 Host 编码尺寸解析、`src/capture/src/capture_module.cpp` 的 `resolve_viewport_encode_dimensions`、`src/ui/gui_shell.cpp` 的 viewport/target-applied 路径。
- 输入：沿 Control input_batch 接收、校验、入队、runtime 消费、geometry/授权、`RemoteInputSession::process` 到 injector 逐段核对。没有逐段计数的步骤应写“缺证”，不靠相邻计数补齐。

## 最高优先级假设：queue 估计为何持续抬高

现有离线 C++ 特征测试在真实网络延迟不增长时，使用 +30 ppm 的相对时钟速率差，160 虚拟分钟后可产生约 287 ms queue 估计并触发严重压力；0 和负向速率差不呈现同样结果。这只是可复现的算法敏感性，**不是已测得现场时钟漂移**。

后续应由执行端从同一 transport sequence 的发送/接收配对样本判断：

- queue 是否随会话年龄平滑累积，而实际 buffered/in-flight、RTT 和消费延迟无相应增长；
- 相对时间差增长斜率能否由稳定的时钟速率差解释，还是与真实等待阶跃、CPU/回调阻塞一致；
- 去重、乱序、反馈新鲜度、速率 revision 切换以及 epoch 重置是否改变样本含义。

发送时间是 send callback 成功返回后的应用时间，接收时间也是应用处理时间；不是网卡/内核出入时刻。毫秒单调时钟乘 1000 不会获得微秒测量精度。先记录这个取证盲区，不能把估计结果包装成单向线延迟。

若真实配对证据确认时钟速率偏差，再最小化修复估计基线/漂移处理，并用恒定延迟、正负 ppm、延迟阶跃、乱序与重连回归验证。若确认真实发送等待，则处理对应队列/准入/调度成本。禁止仅提高阈值、清估计值、定时重启、扩队列或降分辨率掩盖故障。

## 静止模糊单独验收

不等待拥塞复发才查画质。在同一固定文字场景比较源有效尺寸、实际编码尺寸、viewport、显示缩放比例、编码器及目标/实际码率；只有按需、经本机授权的 1:1 对照才能区分缩小与压缩损失。GPU 纹理 padding 不能代替有效画面尺寸。仅有光标闪烁仍可能产生动态帧，不能假设现有静止刷新等于渐进无损恢复。

先定位损失发生在哪一层，再决定尺寸协商、质量预算或静止刷新是否需要最小修复。保持 `stream_video_max_width=0` 原生上限基线；任何诊断降档或配置变化需明确记录，不能作为性能改善结论。

## 完成条件与恢复边界

- Agent：合法授权、真实同步、单次新请求的对端接受/结果和必要审批均可闭环，不能只看本地 dispatched/queued。
- 视频与输入：相同场景下吞吐、帧龄/延迟、队列/估计及可见操作反馈有可重复前后对照；覆盖实际复发时间尺度，短时重启恢复不算修复。
- 画质：固定文字在约定显示比例下通过人工可读性确认，记录有效尺寸及压缩条件。
- 复发后如需恢复工作，保留证据再执行已授权的重连/重启，记录前后实例变化与“缓解而非修复”。
- Release、真实异地回归和未执行的验证必须明确列出。整体联调未结束前不宣称 incident complete；结束时仅停止双方各自已确认的对应定时任务，并取得对端停止回执，不据单个阶段完成自动结案。
