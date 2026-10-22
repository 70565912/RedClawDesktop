# Host 更新重启保护（M10-T05）

Windows GUI Host 在 runtime 运行、已连接且必需数据通道可用时自动开启会话保护。
Controller、等待连接和已断开状态不启用。用户无需在 Controller 配置它。

## 行为

- 管理员启动的 GUI 临时设置 Windows Update 活动时间策略：从当前本地小时起的
  18 小时内不自动重启；已有较小的活动时间上限时遵守该上限。每分钟检查一次，
  跨小时才更新策略，长会话持续覆盖当前时间。
- 同时临时关闭覆盖活动时间策略的两个旧重启策略。保留下载、安装和更新服务配置。
- 主窗口注册关机阻止原因，对可取消的 `WM_QUERYENDSESSION` 返回拒绝。
  Windows 没有在此消息中可靠区分人工关机和更新重启，因此人工普通关机也可能显示
  会话使用提示；断开 Host 后即可正常关机。注销和强制结束会话不拦截。
- 非管理员不会自动提权；日志记录 `administrator_required`，仅有窗口层的关机提示。
- 数据通道断开、runtime 结束、显式停止或 GUI 退出时解除保护并恢复策略原值。

## 原值恢复与并发

写入前把五个策略值的原始存在状态和 DWORD 值保存在机器级应用注册表中。
恢复记录使用显式 `redclaw.update-restart-lease.v1` schema；未知版本或损坏数据拒绝使用。
更新前保留前后可能值，支持进程在部分写入时崩溃后的恢复。下一次管理员 GUI 启动
先恢复遗留记录，再根据新连接重新获取。命名互斥量防止其他 Host 恢复正在使用的记录。

正常恢复仅撤销仍等于本程序写入值的设置；管理员或管理策略已改动/删除的值保留。
刷新检测到外部改动后报告失败，不持续覆盖。日志不输出原值、注册表路径或用户信息。

## 能力边界

这不是对任何 Windows 强制重启的绝对保证。更新截止期限、组织策略或用户选择强制
继续可覆盖保护。已有截止期限时记录 `active_deadline_may_override`，不冒充完全受保护。
Home 版、旧版 Windows 和受 MDM 管理的环境不以注册表写入成功作为实际防重启通过证据。
当前接入是产品的 GUI Host；独立 CLI/runtime 不会自行更改系统更新策略。

依据：[Windows 更新重启策略](https://learn.microsoft.com/en-us/windows/deployment/update/waas-restart)、
[更新截止期限](https://learn.microsoft.com/en-us/windows/deployment/update/update-policies)、
[WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession)。

## 自动验证

`redclaw_ui_connection_flow_tests` 中的 `HostUpdateRestartGuard.*` 和
`UpdateRestartPolicyTest.*` 覆盖跨午夜、重复刷新、原值与缺失值恢复、外部改动、
并发所有权、崩溃遗留恢复、无效记录与原生关机查询。
注册表测试使用独立 HKCU 测试根，主窗口测试注入同一隔离根，管理员测试也不改真实更新策略。
原生查询只发送 `WM_QUERYENDSESSION`，不会实际发起关机、重启或安装更新。
