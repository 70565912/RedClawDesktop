# Remote workspace v0.1.3

Status: approved implementation contract; not yet implemented or released.

The v0.1.2 capture/recovery qualification and release remain the prerequisite delivery. This feature set ships separately as v0.1.3. Task status belongs in [MODULE_KANBAN](../runtime/MODULE_KANBAN.md).

## Product contract

- A valid desktop connection authorizes this connection's file, clipboard and terminal operations. Do not add a second approval, permission checkbox or per-command confirmation. A future connection password is outside this change. Execute under the Host's ordinary current-user permissions without automatic elevation.
- Keep the Controller's system cursor unchanged. Keep the right Agent sidebar separate from the desktop and terminal.
- File transfer supports both directions, multiple files and recursive folders, including empty folders. The remote browser exposes current-user-accessible directories, not only Agent projects.
- Intercept Ctrl+V only when the remote desktop canvas owns keyboard forwarding. Send the current local clipboard snapshot on demand: Unicode text, HTML/RTF, images and actual file/folder lists. Do not continuously synchronize clipboards. Copy semantics never delete the source.
- The terminal is a complete interactive session beneath the desktop, with expandable/collapsible height. It shares the remote desktop session lifecycle; collapsing does not stop its processes and there is no separate terminal-close control.

## Transfer transaction and operation gate

- Use one active transfer batch per connection for file transfer or clipboard transfer. Before transmitting, settle destination and same-name behavior: keep both (default), overwrite or skip.
- Stream files with 64-bit offsets into owned temporary files, verify length and SHA256, then commit each completed file. Partial transfer must not overwrite a previously complete destination. Cancel removes only this operation's incomplete temporary data; completed files remain in the result list.
- Show scanning, current file, direction, completed/total bytes and files, progress and speed. Report receiving and validation separately from submission to the transport.
- During preparation and transfer, reject remote keyboard/mouse input, capture-region changes, terminal input and new/steered Agent work or approval decisions. Enforce the same gate at both endpoints. Keep video, existing program output and Agent messages flowing. Local reading/layout, cancel and disconnect remain available; already-running programs are not suspended.
- Release pressed input when entering the gate. Clear the transfer reason on completion/cancel/failure without clearing independent capture, consent, lifecycle or geometry reasons. Never replay input accumulated during the transfer.
- Cancel and disconnect invalidate the operation's pending paste and late messages. A disconnected batch ends; the user explicitly starts another attempt after reconnection.
- For clipboard paste, withhold the original Ctrl+V until data arrives. Recheck the original focus target, ordinary desktop, session and input eligibility, publish the clipboard and inject one paste. Focus changes or revoked eligibility cancel the pending paste. A delivered key is not proof of application consumption.
- Successful clipboard files remain in a receiver batch directory with open/cleanup actions; do not delete them when merely sending the paste key. Unsupported clipboard formats get an explicit result.

## Terminal and layout

- Host: Windows ConPTY with a persistent Windows PowerShell session. Support native programs/scripts, Unicode/IME, terminal controls, Ctrl+C, history, completion and dimensions. Own and clean up the shell's process tree.
- Controller: an embedded WebView2 surface and bundled xterm.js, with no remote page/resources or general native scripting bridge. Package a fixed WebView2 runtime and its notices; do not require an unrelated Qt upgrade or runtime CDN.
- Terminal text paste uses terminal input and never changes the Host system clipboard. It obeys the transfer gate.
- A transient reconnect within the same desktop session preserves the terminal process and suspends input. Ending the desktop session, closing the remote window or Host exit ends the terminal tree. A new session does not replay old commands.
- Bound terminal output, backpressure and scrollback; never discard live terminal-control bytes in the middle of a stream to hide backlog. Make scrollback eviction visible.
- Replace the navigation overlay with a height-animated layout item above Agent. Preserve usable Agent composer space at small window sizes. Remember navigation height and terminal visibility locally.
- For new messages and streaming deltas in the selected Agent conversation, scroll both the outer conversation and the newest message's inner text view to the final laid-out tail. Without new content, preserve manual history browsing. Other tasks do not steal selection.

## Host self-restart and update

- Provide fixed maintenance actions for restart and update/restart callable from the connected UI or terminal, using the existing independent full-directory upgrade mechanism. Ordinary shell commands do not implicitly acquire independent ownership.
- Before stopping Host, the independent current-user worker must validate and take ownership of the operation, retained launch settings and recovery data. An update additionally stages/verifies the complete candidate bundle. A failed handoff leaves Host running.
- The Controller keeps the remote window open in a maintenance/reconnecting state. The old terminal and initiating Agent may exit; the maintenance worker continues outside their process jobs and outside any replaced directory.
- Preserve the existing connection identity, role and network settings. Confirm startup separately from restored real media. A reconnect creates a new terminal and never repeats the initiating command.
- Restore the previous complete runtime after an update startup failure. A networking failure alone must not cause repeated rollback. An explicit user close suppresses automatic reopening but does not cancel maintenance already handed off.
- Persist a versioned, owner-local operation receipt. Repeated requests are idempotent; incomplete operations are observable after restart. Raw process identity and launch arguments remain local.

## Interfaces, validation and delivery

Introduce bounded typed transfer, clipboard transaction, terminal session and remote-operation-gate components rather than expanding the runtime/GUI entrypoints. Add optional versioned capabilities to existing negotiation. Use separate reliable ordered optional data channels for bulk transfers and terminal traffic; priority cancellation/state messages must not queue behind file data. Bind messages to the session epoch and operation ID; reject stale/repeated commits.

Test both directions, large/empty files, Unicode paths, folders, conflict modes, disk exhaustion, occupied files, changing sources, invalid paths, cancellation races and disconnection. Verify actual received hashes. Test paste in real text, rich-text, image and file applications, including focus changes, repeated keys, cancellation and capture pause. Exercise full terminal applications, flood output, Unicode, resize, collapse and process cleanup. Verify every transfer gate both in GUI and runtime.

Visual acceptance covers small windows, DPI, navigation animation, full Agent text and inner/outer tail scrolling. Compare media cadence, CPU/memory, queue bounds and cancellation latency under simultaneous transfer, terminal and Agent output. Real desktop acceptance requires advancing capture/encode/send/receive/decode/presentation, zero synthetic frames and no decode/presentation failures.

Exercise Host self-restart after forcibly ending the initiating terminal/Agent job, handoff failure, occupied files, corrupt bundles, failed startup rollback, user-close during maintenance and reconnect without duplicate commands. Use isolated directories before physical rolling-upgrade acceptance.

Build Debug and Release serially through build.ps1, run relevant tests and full CTest at the release checkpoint, and list environmental skips. Test both mixed-version directions, package hashes, offline extracted startup with bundled terminal runtime, full-directory rollback and published package smoke. Protect unrelated work, split logical commits, update public documents and use the existing public release repository. Cross-machine Agent work follows the [result contract](../../.agents/skills/remote-agent-result-contract/SKILL.md); each endpoint owns its methods and local evidence.
