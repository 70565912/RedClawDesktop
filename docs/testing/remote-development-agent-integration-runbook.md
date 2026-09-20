# P1-AG-01 remote development Agent integration runbook

This is the Agent scenario catalog for `redclaw-agent-v1`. Follow the [local-first automated matrix](test-matrix.md): protocol, lifecycle, approval permutations, output parsing and UI behavior run locally; local dual endpoints cover both roles. Physical cross-LAN adds one representative authorized real-Provider round trip, not a repeat of every scenario/provider/model below. A fake Provider is not evidence of real account readiness, while a real Provider can be checked locally without a remote operator.

Real account login/consent and operator-assisted scenarios here are developer-selected manual evaluations, not automated sequence items or version-release conditions. Automatic CTests exclude real Provider readiness instead of leaving it skipped. Do not request a manual follow-up solely to close an automatic report.

The bidirectional Agent gate covers both roles. Either role may
invoke the peer; each machine must grant local execution consent for its own
current desktop role. Controller local settings are not a remote login entry.

## 1. Prepare compatible endpoints

When the compressed Protobuf build inputs change, configure/build the affected endpoint
and publish the complete fixed Debug directory; otherwise reuse its validated artifact,
including `redclaw_protocol_codec.exe`, Protobuf and Zstd DLLs. The local PowerShell
Agent API requires a codec compatible with the negotiated wire. During migration,
still-supported Control/Agent text-wire versions must remain interoperable; if no common
mandatory capability exists, report `protocol_version_incompatible` explicitly.
See [wire and migration bounds](../architecture/compressed-protobuf-wire-v1.md).

1. Record each machine's committed Git SHA and product/protocol version. Different supported versions are required to interoperate; do not require matching commits or binaries.
2. Only when a matching artifact is missing or affected sources/dependencies changed, build and publish Debug with `./build.ps1 -Configuration Debug`.
3. Record SHA-256 for both `release/Debug/redclaw_desktop.exe` files as endpoint-specific artifact identities. Independent builds do not need matching hashes; require negotiated common capabilities and successful runtime interoperability instead.
4. Before Runtime starts, open **Development Agent Settings** on the Host. Use only its local Login/Refresh controls; there is no Controller login or API-key field.
5. Select the already-authorized Provider for this scenario. For Codex, confirm `ready`; the probe covers version, official account login status and app-server readiness without starting a model turn.
6. If Cursor/Grok is the selected Provider, its official `cursor-agent` Headless CLI and local login must be ready. Require `ready` plus an applicable model identifier returned by the authenticated account. The Cursor editor alone is insufficient. RedClaw must not install it. Do not require both Providers or another login to repeat a transport check that already passed with one.

The settings dialog keeps account-command output visible and reports start failures,
non-zero exits, cancellation, and timeouts. Do not close or hide the dialog while an
account command is active; cancel it explicitly first. If Codex does not open a browser,
review the local output and run `codex login` in a visible PowerShell window. When the
localhost callback cannot be used, enable device-code login for the account or workspace
and run `codex login --device-auth`, then return to the dialog and click **Refresh**.

## 2. Configure Host authorization

1. In **Development Agent Settings**, register a temporary Git repository containing a deterministic test target. Confirm it is labeled Git clean/dirty; absolute path remains Host-local.
2. Return to the connection page and enable **Allow the connected device to use this computer's development Agent**.
3. Start **Wait for connection**. Confirm the waiting page says Agent access is allowed and the checkbox is locked.
4. Keep remote input independently disabled unless the input scenario is also being tested.

The Controller must receive only an opaque project ID and display name. Reject the run if any absolute Host path appears in the network-facing panel or ordinary log.

## 3. Establish the normal desktop session

Use the ordinary machine-code DHT GUI flow. Before Agent work, prove:

- both endpoints report their own committed Git SHA, product/protocol version, negotiated capabilities, and EXE SHA evidence;
- ICE is connected;
- media, control, and Agent channels are open;
- Host real capture/encode/transmit counters are increasing with `synthetic=0`;
- Controller receive/reassemble/decode/present counters are increasing and a real image is visible.

Agent-channel failure is not allowed to reset the two required channels.

## 4. Codex isolated-worktree task

1. In Controller **Development Agent**, select Codex, the registered project, and **Isolated worktree**.
2. Submit a bounded task that changes one fixture file and runs one focused test.
3. Confirm one `task_id`, live normalized progress, file/tool events, and no provider-private JSON.
4. Accept one requested safe tool/file approval and reject a second request. Confirm the rejected operation does not occur and the task reaches a typed paused/terminal state.
5. Send a follow-up turn on the same RedClaw task. Confirm it resumes only the RedClaw-created Codex thread.
6. Verify the registered project working tree is untouched and the task branch is `redclaw/agent/<task-id>`.

## 5. Disconnect, replay, and interrupt

1. Start a fixture turn that runs for at least 20 seconds and has no pending approval.
2. Start the Debug Controller with `--agent-qa-force-channel-close-after-event`, then start the fixture. The fault fires only after the first task event and closes only `redclaw-agent-v1`; video, control heartbeat, and optional remote input must continue.
3. Let the Host Agent continue producing events while disconnected.
4. After the optional channel reopens, request task sync from the last acknowledged `event_sequence`.
5. Confirm the same `task_id` returns, missing events are replayed once, a gap is explicit if text was evicted, and the turn was not submitted twice.
6. Start another long fixture and interrupt it from Controller. Confirm the provider process stops and the task reaches `interrupted`.
7. Repeat with an outstanding approval: channel loss must reject it and pause the task.

## 6. Cursor/Grok task

Run only when `cursor-agent` readiness is true.

1. Select Cursor and Grok, then submit a second independent isolated-worktree task.
2. Confirm the UI displays `per_turn` approval granularity before the process starts.
3. Reject once and confirm no process work begins; resubmit and approve once. Acceptance starts only that Cursor child process with the official `--trust --force` flags; RedClaw must not persist either permission globally and must not use `--yolo`.
4. Confirm `stream-json` progress is normalized, chat ID remains Host-private, non-zero exit becomes `failed`, and a follow-up uses the registered resume ID.

If readiness is false, record Cursor/Grok as **not executed—official Headless CLI unavailable/not logged in**. Do not mark it passed and do not substitute the editor executable.

## 7. Load and isolation gate

While real video remains active, stream at least 1 MiB of fake or real Agent text and record:

- media decode/present continues;
- control ping/input ACK latency does not show Agent head-of-line blocking;
- Agent DataChannel buffered amount returns below its watermark;
- broker transport queue never exceeds 512 messages;
- turn queue never exceeds eight and the ninth queued request is explicitly rejected;
- task cache stays at or below 4 MiB and total cache at or below 16 MiB;
- any discarded text produces a gap marker, while approval/error/final events remain visible.

The deterministic fake-provider prerequisite is `RemoteAgentBroker.OneMiBTextFloodKeepsEventAndOutboundCachesBounded`; physical/local dual-GUI evidence must additionally prove media/control progress while the Agent channel is loaded.

## 8. Evidence export

Export both endpoints immediately after completion. Preserve:

- Git SHA and Debug EXE SHA-256;
- provider/version/model readiness (no login tokens);
- task IDs, state transitions, event/ACK/gap counts, queue peaks, and durations;
- approval request/decision/timeout counts;
- Agent close/rebuild attempt counts;
- concurrent capture/encode/transmit/receive/reassemble/decode/present counters;
- visible Controller screenshot and test-file/test-result hashes.

Do not export full task instructions, provider credentials, arbitrary local paths, or raw provider JSON. Git commit/push/merge is outside this runbook unless separately authorized.
