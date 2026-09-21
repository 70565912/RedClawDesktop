# RedClaw remote development Agent bridge v1

Status: implemented baseline, physical two-machine provider validation pending
Task: `P1-AG-01`

## Goal and boundary

The bridge lets either desktop endpoint submit a bounded task to an Agent on the peer without taking over existing user sessions. Desktop Host/Controller role controls capture/playback, not Agent request/execution. Video, input and Agent work remain independent.

The first version is intentionally limited to the logged-in Windows user session, one active turn, eight queued turns, registered Host projects, and RedClaw-created provider sessions. It does not expose arbitrary Host paths, Cursor Cloud Agent, service-session login, mobile clients, a second connection password, automatic commit/push/merge, or cross-Agent delegation.

## Transport and envelopes

Application serialization is now typed Protobuf followed by independent whole-message
Zstd compression, sent as binary. GUI/runtime and the local API retain a bounded Base64
line boundary. Both endpoints and the native API codec must upgrade together.
[Wire format, bounds and migration](compressed-protobuf-wire-v1.md).

`redclaw-agent-v1` is a third reliable, ordered WebRTC DataChannel. `redclaw-media-v1` and `redclaw-control-v1` remain required for the desktop session; Agent channel loss never closes media, input, or heartbeat. Only the Host offer side recreates the optional Agent channel, using at most three independent attempts before declaring Agent unavailable until the next normal connection.

Every `AgentMessageEnvelopeV1` carries schema version, connection epoch, strictly increasing message ID, timestamp, task/request IDs, event sequence, message type, and a typed payload. Limits are enforced before send and after parse:

- WebRTC message: 64 KiB maximum.
- UTF-8 instruction: 16 KiB maximum.
- streamed event text: 8 KiB maximum.
- GUI/runtime line: Base64-framed, 64 KiB maximum, and distinguishable from ordinary logs.

Messages cover capability and opaque project catalogs, task creation, follow-up/steer/interrupt, state synchronization, sequenced events and acknowledgements, structured approval decisions, and terminal results. An epoch/replay guard rejects old sessions, duplicates, and reordered commands.

## Host broker ownership

P1-AG-01R makes the broker local to either desktop role. A runtime-owned
AgentPeerSession routes requests (create/start/steer/interrupt/sync/ACK/decision)
to its executor, and capabilities/catalog/snapshot/event/approval/result to
its remote-task client. The session alone stamps wire IDs/epoch; internal
broker IDs are restamped before transmission. Its 64-request/256-result queues
fairly share at most eight sends, checking a one-millisecond soft budget between
sends (a library call/OS scheduling can exceed it; max duration is measured, not
claimed to be a hard deadline). Replay watermarks survive
Agent-only recreation, while desktop epoch reset invalidates them. Only the
desktop offer owner rebuilds the optional channel, under the existing policy.

Host and Controller consent preferences are independent, both initially off:
host/allow_remote_agent and controller/allow_remote_agent. A denied local
executor can still invoke an authorized peer. Both roles expose the current-user
API with independent names/journals for local dual GUI. Host has a modeless
window using the same conversation widget; hiding it never stops work.
Implementation acceptance is covered by the bidirectional Agent integration runbook.

`RemoteAgentBroker` owns task and provider lifecycle outside `run_runtime_mode`:

- one active turn and FIFO queue of at most eight;
- at most 20 task records;
- 4 MiB event cache per task and 16 MiB total;
- a separate 512-message transport queue;
- text deltas are discarded first, with a gap snapshot instructing the Controller to resynchronize;
- approval, error, state, and terminal events are preserved preferentially;
- event ACK plus `agent_task_sync_request` resumes from a Controller sequence without re-executing a turn.

If connectivity is lost while a task has no pending approval, the Host process and Agent turn continue. For older peers or the legacy-approval test fixture, pending approvals are rejected and the task is paused; those requests also fail closed after 60 seconds. Current production providers never create a pending approval. Host shutdown interrupts provider processes. Only non-sensitive completed-task metadata—task ID, provider, model, opaque project ID, work mode, terminal state, and last sequence—is persisted; instructions, output, local paths, and credentials are not stored in that metadata file.

## Restart and unavailable history

After a Host process restart, completed-task metadata does not contain event text.
The Broker sends an explicit `agent_task_snapshot` with `gap=true`,
`event_kind=history_gap`, `error_code=history_unavailable`, and the last lost
event sequence before the retained terminal-state snapshot. The Controller
journals that loss boundary before advancing ACK. This acknowledges a disclosed
gap, not receipt or display of the missing text. Replayed loss boundaries are
not displayed or journaled repeatedly; ordinary high-sequence snapshots still
cannot skip missing events. Current pending approvals retain their existing
replay/decision handling.

For a task no longer retained, a sync request receives a correlated snapshot:
`error_code=task_not_found`, `event_kind=sync_unavailable`, `complete=true`,
`task_state=failed`, the original task/request IDs, and the request's ACK
watermark (no new event is invented). The Controller resolves only a matching
pending sync at its current watermark. It preserves an already-known terminal
result, marks an unresolved nonterminal task unavailable, and does not execute
or resume it. Other pending tasks remain gated. These are semantics of the
existing envelope, not a new wire version; both endpoints need the fix to
eliminate the metadata-only restart loop.

## Project and work-directory policy

The Host GUI stores a local list of project roots in the dedicated **Development Agent Settings** dialog. At wait startup it writes an owner-local manifest with a random persistent `project_id`, display name, and canonical local root. Only `project_id`, display name, and the non-sensitive Git/non-Git capability cross the network. A non-Git registration is restricted to `direct_workspace`; the Controller cannot select `isolated_worktree` for it.

`isolated_worktree` is the default. It creates `redclaw/agent/<task-id>` from the registered repository's current `HEAD` beneath a sibling `.redclaw-agent-worktrees` directory. `direct_workspace` uses the registered root as-is. The bridge never stashes, resets, removes dirty worktrees, commits, pushes, or merges on its own.

## Providers

### Codex

The Host starts `codex app-server --listen stdio://`, performs `initialize/initialized`, and maps RedClaw lifecycle to `thread/start`, `thread/resume`, `turn/start`, `turn/steer`, and `turn/interrupt`. Readiness requires `codex --version`, `codex login status`, and an app-server help probe. A locally installed older CLI that rejects the desktop configuration value `service_tier=priority` is retried with a process-scoped `service_tier="fast"` override; RedClaw never rewrites the user's Codex configuration. The working directory is restricted to the selected project/worktree, sandbox policy is `workspace-write`, and `approvalPolicy=never` is sent with `thread/start`, `thread/resume`, and every `turn/start`. Submitted work runs without command or file approval prompts. An unexpected app-server approval request is declined automatically with a typed diagnostic; failure to send that reply terminates the task as failed. Provider-private JSON never crosses WebRTC. The policy follows the [official app-server contract](https://developers.openai.com/docs/app-server).

### Cursor and Grok

Only the standalone `cursor-agent` Headless CLI qualifies. The Cursor editor executable is not treated as a provider. Readiness requires version, account-status, and model-list probes. Grok model identifiers are parsed from the authenticated account's actual `cursor-agent models` output; there is no hard-coded or freely editable model field. Submitting a task or follow-up starts a turn directly with `--print --output-format stream-json --stream-partial-output --trust --force`, the selected published `--model`, and registered `--resume` chat IDs. These trust/command flags are scoped to that child process; RedClaw does not persist a global Cursor permission and does not use the `--yolo` alias.

Cursor has no per-turn approval queue. A failed initial process launch can be retried with its registered workspace and model; a previously launched task must use its registered chat ID rather than silently losing history. Mid-turn steer is reported unsupported; interrupt remains available. RedClaw never downloads a CLI or accepts an API key. An idle Host user may explicitly click Login/Logout in the local settings dialog; RedClaw then launches the official CLI browser-account flow, discards raw authentication output, applies a ten-minute timeout, and never exposes the operation to Controller.

Both current providers advertise `supports_structured_approval=false` and `requires_turn_approval=false`. Agent v1 approval messages and Controller cards remain supported for older execution peers; upgrading only the Controller does not change an older Host's approval policy. No protocol version changes are required.

Both providers publish one typed readiness value: `cli_missing`, `not_authenticated`, `ready`, `probe_failed`, or `model_unavailable`. A zero exit code accompanied by a logged-out status, abnormal output, or timeout remains fail closed. Host settings can force a fresh probe; account and project mutations are locked while Runtime is active.

## Authorization and diagnostics

Host authorization is a separate wait-page checkbox, persisted with `QSettings("RedClaw", "RedClawDesktop")`, default off, and locked for the active wait session. Runtime/profile equivalents are `allow_remote_agent=false`, `--allow-remote-agent`, and `--agent-project-manifest`. Missing or invalid project registration fails Agent closed without taking down desktop viewing.

Normal logs record only task ID, hashed/omitted instruction identity, provider/version/model, state, event count, duration, and redacted errors. Broker and channel telemetry records capability refreshes, task/event/ACK/replay/gap/approval totals, queue/cache peaks, independent channel close/rebuild, and duplicate-task suppression without credentials, Host paths, or full instructions. Debug QA accepts only predefined Agent fixture IDs and a `codex|cursor` selector; it has no arbitrary-prompt escape hatch. A Debug-only fault switch closes only the Agent channel after the first task event, and a fake-provider regression emits 1 MiB of bounded text to verify eviction and gap behavior.

## Coordination cutover layer

`P1-AG-02` adds a current-user Controller API on top of the same
`AgentMessageEnvelopeV1`; it does not add a second task protocol. The API
validates the authenticated Provider/model/project catalogs, connection epoch,
increasing local message ID, task/request identity, work-directory mode, and
sync barrier before forwarding to `redclaw-agent-v1`. Typed evidence references
carry only a manifest basename and SHA-256.

An owner-local append-only journal reconstructs request, reply, supersession,
ACK, terminal, evidence, Git, and executable identity without storing prompts,
paths, credentials, signaling, or logs. Its authority is exactly one of
`none`, `normal_agent`, `debug_bridge`, and `out_of_band`; normal requests
reload this state before send. An independent current-user lifecycle supervisor
owns approved formal publish/stop/restart and survives replacement of the GUI.
Recovery is an explicit operator action and never reads executable work from
the source repository.

## Acceptance boundary

Unit and fake-process tests establish parser bounds, epoch/replay isolation, bounded caches/queues, queueing, follow-ups, direct task execution and noninteractive policy on start/resume/turn, legacy approval rejection/timeout, provider protocol normalization, crash/invalid readiness behavior, and completed metadata reload. Product acceptance additionally requires the physical two-machine procedure in `docs/testing/remote-development-agent-integration-runbook.md`, including mixed-version negotiation across all still-supported releases, Debug configuration, and measured interoperability; matching committed source is not a gate, and independently built executable hashes are recorded separately. Real video must progress throughout a real Codex worktree task and—only after official CLI install/login—a separate Cursor/Grok task.
