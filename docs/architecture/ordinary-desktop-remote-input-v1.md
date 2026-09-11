# Ordinary Desktop Remote Input v1

`P0-DS-09` adds explicitly authorized keyboard and mouse control to the ordinary logged-in Windows desktop. It reuses the reliable ordered `redclaw-control-v1` DataChannel and does not create another transport channel.

This version does not cross Windows integrity or desktop boundaries. Administrator/elevated windows remain subject to UIPI, and UAC, the sign-in screen, the secure desktop, Ctrl+Alt+Del, game-style relative mouse input, clipboard, and mobile input are outside this contract.

## Authorization and states

The Host connection page owns the local authorization decision. `QSettings("RedClaw", "RedClawDesktop")` stores `host/allow_remote_control` for the current Windows user; the first-run value is `false`. Entering Host wait snapshots the checkbox into the runtime launch as `--allow-remote-input`, locks the checkbox, and shows either remote control allowed or view only. Automatic reconnection keeps that wait-cycle authorization. Manual stop or process exit destroys the runtime grant without deleting the stored preference.

Runtime profiles accept `allow_remote_input=true|false`; the default remains false for CLI, automation, Debug, and Release. Authorization belongs only to the current runtime connection and control epoch. It is not bound to a Controller fingerprint in v1.

Host reports one of `unavailable`, `available`, `active`, `paused`, or `denied`, with typed reasons including unsupported, not authorized, no video, local pause, lease expiry, queue overflow, injection failure, disconnect, geometry change, and stale sequence. Missing input capability messages are interpreted as view-only compatibility.

## Protocol messages

Every input message uses the normal control envelope: schema version, session epoch, increasing message ID, and timestamp. The input extension adds:

| Message | Direction | Purpose |
|---|---|---|
| `input_capabilities` | Host to Controller | Support, authorization, state, capture origin/size/rotation/revision |
| `input_control_request` | Controller to Host | Explicit start or stop request |
| `input_control_status` | Host to Controller | State/reason and last applied input sequence acknowledgement |
| `input_batch` | Controller to Host | Up to 64 typed events and at most 4 KiB |
| `input_state_sync` | Controller to Host | Pressed scan codes/button mask every 500 ms |
| `input_release_all` | Controller to Host | Immediate fail-closed release and pause |

`RemoteInputEventV1` carries a scan code, virtual key, extended and repeat flags, normalized coordinates, mouse button, and vertical or horizontal wheel delta. Validation rejects zero/out-of-range scan codes, inconsistent per-event fields, invalid buttons/wheels, more than 64 events, oversized batches, stale epoch/message IDs, and non-increasing input sequences. The protocol and diagnostics never log individual key values or text.

## Controller capture

The Controller button requests Host activation first. An `active` status enables the user's control intent and installs the Windows `WH_KEYBOARD_LL` hook. Control intent and current forwarding eligibility are separate: `control_enabled` remains true until explicit Stop, the emergency shortcut, disconnect/epoch change, authorization loss, lease/queue/ACK failure, injection failure, or control-channel failure; `input_forwarding` is true only while the playback window is visible, restored, foreground, and outside a geometry transaction. Window deactivation, hide/minimize, and geometry changes combine as `window_inactive`, `hidden_or_minimized`, and `geometry_transaction` suspension flags. Clearing every flag automatically resumes forwarding without another Host request.

Every non-injected hook event rechecks the playback foreground-window identity. While locally suspended it is passed through to Windows and never queued remotely. The hook preserves scan code and extended-key identity, marks genuine auto-repeat, and suppresses captured keys locally so Alt+Tab and Windows keys reach the Host only while forwarding is enabled. Host keyboard layout and IME interpret the physical-key stream; local Unicode/IME commits are not transported.

`Ctrl+Alt+Shift+Esc` is a Controller-local emergency exit and is never sent. Ctrl+Alt+Del is suppressed from the remote protocol because ordinary `SendInput` cannot implement it. RedClaw does not take an explicit process-wide mouse grab, so the Stop Control button and the rest of the local UI remain directly usable.

Mouse coordinates are calculated against the latest successfully displayed frame's aspect-fit content rectangle. Movement, button-down, double-click, and wheel events outside that rectangle, including letterbox bars and other Controller UI, are never sent and do not alter control intent. A drag that began inside may deliver its matching ButtonUp after the pointer leaves only as state cleanup: it uses the last valid in-content coordinate and does not move the Host pointer. This exception prevents a stuck remote button without turning outside-window activity into remote input. On Windows, spontaneous candidate Qt mouse events read the current message's [`GetMessageExtraInfo`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmessageextrainfo) value before queuing; movement, button, double-click, release, and wheel events carrying RedClaw's shared `SendInput` marker are consumed locally. Physical events and other marker values continue normally. This breaks same-session Host-to-Controller mouse feedback without adding a global mouse hook.

Entering a local suspension clears unsent events and locally tracked keys/buttons, immediately sends an empty `input_state_sync`, and continues empty syncs every 500 ms. Host releases its tracked input but remains `active`, so the three-second lease does not expire during a long minimize or focus loss. `input_release_all` is reserved for a real terminal transition. Reconnect never automatically restores Controller control intent.

When control is disabled, a mouse press on the playback canvas is consumed locally. One mouse-transparent, focus-free overlay displays a red prohibition symbol for two seconds, with either “未开始控制，请点击 Start Control” or the view-only reason. Repeated presses only restart the same timer. The overlay is an independent frameless surface, so D3D11/OpenGL/Qt canvas geometry and swap-chain resize counts do not change. `blocked_control_click_hint_total` records only the count.

## Host mapping and injection

Capture frames advertise the source desktop origin, dimensions, rotation, and geometry revision. Controller coordinates use `0..65535`. Host maps them to the current capture rectangle, including negative virtual-desktop coordinates and 0/90/180/270-degree rotation, before the Windows backend converts the physical point to `SendInput` virtual-desktop absolute coordinates.

Keyboard injection uses `KEYEVENTF_SCANCODE` and `KEYEVENTF_EXTENDEDKEY`; mouse injection uses batched `SendInput`, `MOUSEEVENTF_ABSOLUTE`, and `MOUSEEVENTF_VIRTUALDESK`, including X1/X2 and both wheel axes. Every injected event carries the public `redclaw::input::kRedClawInputExtraInfo` marker in [`dwExtraInfo`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-mouseinput), so the keyboard hook and Controller mouse guard can identify feedback from the same RedClaw injection contract. A partial `SendInput` result triggers compensating key/button releases and fail-closed pause.

Network callbacks only validate and enqueue. The runtime loop owns the input session and processes it before heartbeat, viewport/statistics, and remote-log output. Critical Controller events use a 256-entry FIFO; mouse move is latest-only and flushed at no more than about 60 Hz. Host has a 256-event queue. Overflow, invalid mapping, injection failure, or a control-channel backlog at or above 64 KiB pauses input instead of dropping KeyUp/ButtonUp silently. Remote log chunks are withheld while input work is pending.

The Controller sends a full pressed-state sync every 500 ms. Host only uses it to release locally tracked states that the Controller no longer reports; it never invents a press from a sync message. A three-second lease releases all tracked keys/buttons and pauses the session. Disconnect resets the input sequence for the next epoch.

Debug QA uses the same lifecycle instead of a second injection path. The current-user local control pipe can request Start/Stop Control, one normalized paired mouse click, or one paired scan-code key press. Start still requires Host authorization, open control channel, and a real displayed frame; key QA additionally requires current forwarding eligibility. Queue/ACK pressure, denial, disconnect, or send failure ends control and releases all input, while local window suspension only emits empty state sync. Responses expose `control_enabled`, `input_forwarding`, combined suspension reasons, the blocked-click count, queue/counter deltas, loopback suppression, and acknowledgement RTT; neither side logs the requested key value or text. Host runtime logs then provide count-only confirmation that a batch or empty sync was applied.

## Telemetry and validation boundary

Host heartbeat telemetry records received/rejected batches, injected event count, release-all count, queue peak/current depth, last applied sequence, and capabilities/status totals. Controller status logs are rate-limited and record sent batches, coalesced moves, suppressed same-session mouse-loopback events, blocked-click hints, acknowledgement RTT, and a bounded rolling P95 without recording key content. Debug `tail_log` reads the already-redacted 4096-line/1 MiB `ProcessFileLogger` memory ring rather than scanning `QPlainTextEdit`; it returns at most 200 lines with a 64 KiB snapshot limit and a 48 KiB JSON log-array budget, plus elapsed/truncated fields. There is no unbounded GUI-text fallback.

Validation covers protocol, runtime-profile, input-session, connection-flow/UI, and bounded-log behavior plus standard Debug and Release builds. Local dual-GUI acceptance must prove real capture, encoded transport, hardware presentation, authorized input, minimized-window suspension, forwarding recovery, explicit Stop, and bounded diagnostic export. Physical two-machine input, forced disconnect, and measured LAN acknowledgement latency remain separate acceptance evidence; elevated and UAC targets require their dedicated runbook.
