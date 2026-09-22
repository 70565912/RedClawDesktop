# 连接密码 v1

Host 保存本机连接密码后才可等待连接。Client 输入 Host 机器码和对应密码，验证通过后自动连接，不出现人工批准。Client 只在成功后记住该机器码的密码；输入框可直接修改，Forget 删除保存值。错误密码保留机器码、清空密码并停止自动重试；在线 Host 拒绝该次连接后重新等待。

首页左列为本机连接码、密码及等待按钮，右列为对方连接码、密码及连接按钮；权限、Agent 和网络选项集中在默认折叠的 More settings，原有授权值不变。密码与连接码输入共用样式，首页只保留一个状态提示区。

本机密码首次设置和修改均只输入一次，点击 Save 异步保存。存在未保存输入时禁止等待连接，保存成功或清空输入后恢复。保存成功会清空输入并恢复隐藏；已设置时显示“Set; enter a new password to change”，不填充假密码。本机仍只保存校验材料，Show 只能显示当前输入，不能恢复原密码。对方密码可显示当前输入或已记住的密码，Forget 删除记忆。两个输入默认显示星号，内嵌 Show / Hide 切换保留光标和选区；切换对方连接码、忘记密码或开始连接时恢复隐藏。等待或连接期间不可修改；先退出当前会话。密码非空，保留首尾空格、区分大小写，无复杂度要求。协议上限为 1024 UTF-8 字节，不接受 NUL。界面默认隐藏密码，派生校验材料在后台完成。

## 凭据与本地启动

Windows 当前用户 DPAPI 加密保护本机 Host 的 SCRAM 校验材料，以及 Client 按机器码保存的认证凭据。QSettings 的 `connectionAuth/v1/host`、`connectionAuth/v1/peers/<code>` 只包含密文。解密失败必须重新设置/输入，不使用空密码。

GUI 启动 Runtime 后通过其私有 stdin 管道发送一次性 `RCD-LOCAL-AUTH-V1` 本地消息；Runtime 在启动网络前读取并验证角色和版本。该消息不是网络控制消息，不进入网络转发、普通配置、维护重启参数、诊断或日志。后续启动/重连仍由 GUI 提供凭据。

无人值守脚本通过当前用户 DPAPI 加密文件提供相同本地消息。文件路径可作为 `--connection-credential-file` 参数，密码和校验材料不可作为参数。GUI 使用文件时仍通过私有管道交付 Runtime。示例（交互读取隐藏密码）：

```powershell
.\scripts\service\new-connection-credential.ps1 -Role host -OutputPath build\host.dpapi
.\scripts\service\new-connection-credential.ps1 -Role controller -OutputPath build\controller.dpapi
```

在两次提示中输入相同密码，给相应启动脚本传入 `-ConnectionCredentialFile`。自动化可使用 `SecureString` 的 `-Password` 参数，不应在脚本、命令行或转录日志中写入明文密码。密文文件属于本地生成物，不提交；跨用户/机器不可直接复用。双 GUI 测试脚本自动生成一次性测试密码并保护两份角色凭据。

## 协议与门禁

独立 `ConnectionAuthenticator` 状态为待验证、验证中、通过、拒绝。使用 [RFC 7677](https://www.rfc-editor.org/rfc/rfc7677.html) 和 RFC 5802 的 SCRAM-SHA-256 密钥/证明计算，PBKDF2-HMAC-SHA256 120000 次、随机 16 字节 salt、32 字节 StoredKey/ServerKey。Host 不保存原始密码。

这是 RedClaw 连接认证 profile，不宣称通用 SASL 互操作：密码按原样 UTF-8 处理；Control 通道内交换 client-first/server-first/client-final/server-final，附加双向 finished/accepted 确认。证明绑定双方随机 nonce、Host/Controller 角色、该连接双方 DTLS SDP 指纹以及各自随机会话 epoch。相同密码在新连接上仍需重新证明，旧证明不能重放。

Protobuf 保留所有原字段编号，追加消息类型 `kConnectionAuth=26` 和字段 `connection_auth_version=85`、`auth_step=86`、`auth_data=87`。Hello 协商认证版本 1，缺少共同版本明确报告 `protocol_version_incompatible` 并提示升级。新端绝不与无密码旧端建立业务会话；两个方向均按此要求拒绝。不要求相同 Git commit 或二进制。

DataChannel 物理打开只表示传输就绪。统一传输门禁在双向证明通过前阻止 Media、Control 业务、Agent、Navigation、DebugBridge、Terminal、Transfer 的发送和业务回调。输入、剪贴板、文件及远程日志沿这些通道受到相同约束；提前业务消息丢弃，不缓存执行。密码通过不会授予输入、Agent 或其他额外功能权限。

每次 retire/rebuild 清除上一连接认证状态，保持 Host 失败计数。验证限时 30 秒，失败终止当次连接。连续错误 5 次后冷却 30 秒，冷却中拒绝新认证；成功会话不被冷却中断。在线 DHT Host 使用现有 owner 线程恢复路径立即发布新 standby Offer；尚未认证的 Client 可采用同一 Host 严格更新的 generation，避免卡在拒绝前缓存的旧 Offer。已认证会话仍保持原有重连约束。file signaling 仅用于诊断，不作为在线恢复验收替代。

## 验证入口

- `redclaw_connection_auth_tests`：密码、DPAPI、篡改、重放、超时、重连、密码修改、冷却。
- `redclaw_connection_auth_integration_tests`：真实本地 ICE/DTLS 通道，所有业务通道门禁与 legacy 能力双向拒绝。
- `redclaw_ui_connection_flow_tests` 中 `ConnectionEntryPage.*`：单次输入、空值拒绝、星号/明文切换及光标、未保存修改门禁、异步保存失败、当前用户密文、成功后记忆、忘记、解密失败和运行期禁改。
- `run-local-dual-gui-integration-test.ps1`：安全启动凭据、真实采集和 GUI 呈现。
- `run-connection-password-validation.ps1 -LegacyRuntimeExe <旧程序路径>`：测试自有进程中的错误密码、实际旧二进制双向拒绝，以及同一在线 Host 拒绝错误密码后接纳正确 Client。保留运行中的用户实例。

实际构建、测试报告及尚未完成的验收以 [DEVLOG](../logs/DEVLOG.md) 和 [看板 X00-T23](../runtime/MODULE_KANBAN.md) 为准。源码测试模拟旧线协议与真实旧二进制验证应分别记录。
