# M07-T03 Handoff Integration Runbook

本手册用于人工联调与证据留存，覆盖 M07-T03 的三条关键链路：
- 登录升级（pre-login -> user-session）
- 登出降级（user-session -> pre-login）
- helper 崩溃恢复（预算内恢复 / 预算外回落）

## 适用范围
- 模块：M07（与 M06/M09 能力联动）
- 目标：验证 handoff 行为稳定、可恢复、可审计
- 当前平台：Windows（VS2022 + CMake preset）

## 前置条件
- 已完成 Debug 构建。
- 在仓库根目录运行命令。
- PowerShell 可执行 `ctest`。
- 关键测试目标存在：
  - `redclaw_m07_handoff_integration_e2e_tests`
  - `redclaw_m07_logout_fallback_integration_tests`
  - `redclaw_m07_helper_crash_recovery_integration_tests`

## 快速执行（推荐）
```powershell
cmake --preset vs2022-x64
cmake --build --preset debug --target redclaw_m07_handoff_integration_e2e_tests redclaw_m07_logout_fallback_integration_tests redclaw_m07_helper_crash_recovery_integration_tests redclaw_m07_handoff_orchestration_integration_tests redclaw_session_handoff_state_machine_tests
ctest --test-dir build\vs2022-x64 -C Debug --output-on-failure -R "redclaw_m07_handoff_integration_e2e_tests|redclaw_m07_logout_fallback_integration_tests|redclaw_m07_helper_crash_recovery_integration_tests|redclaw_m07_handoff_orchestration_integration_tests|redclaw_session_handoff_state_machine_tests"
```

## 场景 A：登录升级
1. 运行 `redclaw_m07_handoff_integration_e2e_tests`。
2. 关注日志关键点：
   - 检测到 logon 事件。
   - helper 启动成功。
   - capability sync 成功并进入 user-session。
3. 通过标准：
   - 状态机最终为 `kUserSessionActive`。
   - helper 运行态为 true。

## 场景 B：登出降级
1. 运行 `redclaw_m07_logout_fallback_integration_tests`。
2. 关注日志关键点：
   - 检测到 logoff 事件。
   - helper 停止成功。
   - capability sync 降级到 pre-login。
3. 通过标准：
   - 状态机最终为 `kPreLogin`。
   - helper 运行态为 false。

## 场景 C：helper 崩溃恢复
1. 运行 `redclaw_m07_helper_crash_recovery_integration_tests`。
2. 关注日志关键点：
   - helper 丢失后进入 downgrading。
   - 预算内恢复路径可回到 `kUserSessionActive`。
   - 预算外失败路径回落 `kPreLogin`。
3. 通过标准：
   - 两条子路径都通过断言并返回 PASS。

## 审计日志检查
- handoff 状态机会输出结构化审计字段：
  - `session_id`
  - `from`
  - `to`
  - `event`
  - `transitioned`
  - `error`（失败路径）
  - `timestamp_unix`
- 建议至少保留一条成功迁移和一条失败迁移样例。

## 失败排查
- 若 capability sync 未进入 synchronized：
  - 先检查 IPC channel 是否处于 connected。
  - 再检查 advertisement/ack 序列号与 accepted 标志。
- 若状态机迁移失败：
  - 检查事件顺序是否符合 `SessionHandoffStateMachine` 规则。
- 若 helper 无法停止或重启：
  - 检查 launcher adapter 进程运行态与 session_id 对应关系。

## 证据留存建议
- 保存 `ctest --output-on-failure` 原始输出。
- 记录本次运行信息：
  - commit hash
  - Windows 版本
  - 运行时间（UTC）
  - 测试结果汇总（通过/失败）
- 在 handoff 交接时附上审计日志样例与失败重现步骤（如有）。
