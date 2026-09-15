# RedClawDesktop 开发文档

首次了解、构建或接手项目时，先阅读根目录 [README.md](../README.md)。它是唯一强制入口；本页提供按需展开的开发文档导航。

## 构建与贡献

- [贡献指南](../CONTRIBUTING.md)
- [编码约定](setup/coding-conventions.md)
- [本机配置](setup/local-machine-config.md)
- [CMake Tools 故障排查](setup/cmake-tools-api-failure-troubleshooting.md)
- [vcpkg 与 FFmpeg 本地化故障排查](setup/vcpkg-ffmpeg-locale-troubleshooting.md)
- [PowerShell 脚本签名与 lint](setup/powershell-script-signing-and-linting.md)
- [v0.1.1 Release notes](releases/v0.1.1.md)
- [v0.1.2 候选发行说明](releases/v0.1.2.md)
- [v0.1.0 Release notes](releases/v0.1.0.md)

发布 Windows x64 便携包使用：

```powershell
.\scripts\release\publish-github-release.ps1 -Version 0.1.1 -PackageOnly
```

去掉 `-PackageOnly` 后，脚本会校验干净的公开 `main`、创建注释标签并发布 GitHub Pre-release。

## 架构

- [解决方案架构](architecture/solution-architecture.md)
- [DHT rendezvous 与本地 helper](architecture/serverless-dht-rendezvous-and-local-helper-design.md)
- [可靠连接协商](architecture/reliable-fast-connection-negotiation-v2.md)
- [压缩 Protobuf 线格式](architecture/compressed-protobuf-wire-v1.md)
- [自适应桌面流控制](architecture/adaptive-desktop-stream-control-v1.md)
- [DirectX 采集兼容性](architecture/directx-capture-compatibility-design.md)
- [采集恢复与 Host 光标](architecture/capture-recovery-and-cursor.md)
- [v0.1.3 文件、剪贴板、终端与自重启实施约定](architecture/remote-workspace-v013.md)
- [普通桌面输入](architecture/ordinary-desktop-remote-input-v1.md)
- [远端 Agent 桥](architecture/remote-development-agent-bridge-v1.md)
- [P2P Debug Bridge](architecture/p2p-debug-bridge-v1.md)
- [无人值守与开机接入](architecture/unattended-boot-access-design.md)

## 测试与联调

- [测试矩阵](testing/test-matrix.md)
- [双机桌面流运行手册](testing/p0-dual-machine-desktop-stream-runbook.md)
- [跨 LAN 双机联调](testing/cross-lan-dual-machine-integration-playbook.md)
- [画面排队迟缓与静止模糊诊断交接](testing/desktop-latency-quality-investigation-20260915.md)
- [独立运行目录升级](testing/runtime-directory-upgrade.md)
- [远端 Agent 联调](testing/remote-development-agent-integration-runbook.md)
- [Debug Bridge 联调](testing/p2p-debug-bridge-integration-runbook.md)
- [产品性能基线](testing/product-performance-baseline-v1.md)
- [GitHub Actions 验证范围](testing/github-actions-validation.md)
- [特权输入与 DirectX 检查表](testing/e2e-privileged-directx-checklist.md)

本地或双机证据应写入被 Git 忽略的 `build/reports/` 或 `reports/`。运行信令、密钥、机器路径、真实网络地址和完整日志不得提交。

## 项目执行状态

- [当前状态](runtime/PROJECT_STATE.md)
- [任务看板](runtime/MODULE_KANBAN.md)
- [Agent 执行协议](runtime/AGENT_EXECUTION_PROTOCOL.md)
- [模块规格](modules/module-specs.md)
- [阶段日志](logs/DEVLOG.md)
- [v0.1.1 发布说明](releases/v0.1.1.md)
- [v0.1.0 发布说明](releases/v0.1.0.md)

## AI coding 配置

- [仓库 Agent 规则](../AGENTS.md)
- [远端自主执行与结果回执 Skill](../.agents/skills/remote-agent-result-contract/SKILL.md)
- [文档维护规则](../.github/instructions/docs-tree-maintainer.instructions.md)
- [计划模板](../.github/prompts/plan-template.prompt.md)

任何 GitHub 联网操作都必须先向用户说明，并取得当前任务中的明确授权。
