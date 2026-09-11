# M07-T03 Pre-login 到 User-Session Handoff 设计

## 范围
- 目标：在 Windows 上实现 `Session 0` 服务进程与用户会话 helper 之间的可恢复切换。
- 覆盖流程：
  - 登录升级：`PreLogin -> LaunchingHelper -> SyncingCapabilities -> UserSessionActive`
  - 登出降级：`UserSessionActive -> Downgrading -> PreLogin`
  - helper 异常恢复：有界重启，超预算后回落到 `PreLogin`
- 不在本任务范围：
  - 真实 Windows token/process 启动实现细节（当前保留 adapter 抽象）
  - 跨平台实现（当前仅 Windows）

## 设计目标
- 会话连续性：用户登录/登出或 helper 异常时，远程会话不出现不可恢复中断。
- 安全边界：服务进程（高权限）与 helper（用户桌面上下文）职责分离。
- 可验证性：关键状态迁移均可通过单测/集成测试覆盖。
- 可审计性：handoff 关键事件具备结构化审计日志输出。

## 架构与职责

### 组件
- `service::SessionDetectionListener`
  - 监听会话事件（如 `WTS_SESSION_LOGON` / `WTS_SESSION_LOGOFF`）。
- `service::HelperProcessLauncher`
  - 管理 helper 启停、运行态查询、异常停止后的重拉起。
- `service::IpcCapabilitySyncCoordinator`
  - 通过 IPC 广告/确认完成能力协商（升级/降级）。
- `session::SessionHandoffStateMachine`
  - 维护 handoff 状态机和合法迁移规则。
- `helper::HelperBootstrap` / `helper::HelperCapabilityManager`
  - helper 启动、握手、能力注册、keepalive。

### 状态机
- 状态：`kPreLogin`, `kLaunchingHelper`, `kSynchronizingCapabilities`, `kUserSessionActive`, `kDowngrading`, `kFailed`
- 关键事件：`kInteractiveSessionDetected`, `kHelperLaunchSucceeded/Failed`, `kCapabilitySyncSucceeded/Failed`, `kUserLoggedOut`, `kHelperLost`, `kDowngradeCompleted`, `kResetToPreLogin`

## 主流程

### 登录升级
1. 监听到 `logon` 事件。
2. 状态机从 `kPreLogin` 迁移到 `kLaunchingHelper`。
3. 启动 helper，成功后进入 `kSynchronizingCapabilities`。
4. 能力同步成功后进入 `kUserSessionActive`。

### 登出降级
1. 监听到 `logoff` 事件。
2. 状态机从 `kUserSessionActive` 迁移到 `kDowngrading`。
3. 停止 helper，能力降级到 `PreLogin`。
4. `kDowngradeCompleted` 后回到 `kPreLogin`。

### helper 崩溃恢复
1. 检测到 helper 丢失（`kHelperLost`）进入 `kDowngrading`。
2. 按有界重试预算执行重启。
3. 若重启成功，走升级路径回到 `kUserSessionActive`。
4. 若重启失败，完成降级并停留 `kPreLogin`。

## IPC 与能力协商
- 通道：`WindowsNamedPipeIpcChannel`（当前默认 in-memory adapter 可测试）。
- 消息：握手、capability advertisement、capability acknowledgment、keepalive、shutdown。
- 能力模型：
  - `PreLogin`：受限能力集合
  - `UserSession`：用户桌面能力集合

## 审计日志
- handoff 状态机输出 `HandoffAuditEvent`，字段包含：
  - `session_id`, `from`, `to`, `event`, `transitioned`, `error`, `timestamp_unix`
- 通过 `format_handoff_audit_log_line(...)` 统一格式化，便于归档与检索。

## 里程碑
- M1 IPC 基础与能力协商接口落地。
- M2 helper bootstrap/lifecycle/capabilities/launcher 落地。
- M3 session detection + handoff 状态机落地。
- M4 orchestration / handoff / logout-fallback / crash-recovery 集成测试落地。
- M5 handoff 审计日志补齐并通过测试验证。

## 依赖
- M07-T01 服务生命周期封装。
- M07-T02 开机前登录场景基础能力。
- `docs/architecture/unattended-boot-access-design.md`
- `docs/architecture/solution-architecture.md`

## 风险与缓解
- 会话事件与 helper 生命周期竞态：
  - 使用状态机约束迁移，拒绝非法事件序列。
- helper 重启抖动导致资源消耗：
  - 引入有界重启预算并在失败后降级。
- IPC 中断导致能力状态不一致：
  - 失败路径显式进入 `kFailed` 或 `kDowngrading`，禁止静默忽略。

## 验收标准
- 登录后可稳定升级到 `UserSessionActive`。
- 登出后可稳定回退到 `PreLogin`。
- helper 崩溃可在预算内恢复，预算外正确回落。
- handoff 关键迁移可输出结构化审计日志。
- 以下测试通过：
  - `redclaw_session_handoff_state_machine_tests`
  - `redclaw_m07_handoff_orchestration_integration_tests`
  - `redclaw_m07_handoff_integration_e2e_tests`
  - `redclaw_m07_logout_fallback_integration_tests`
  - `redclaw_m07_helper_crash_recovery_integration_tests`
