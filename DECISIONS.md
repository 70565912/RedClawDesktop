# Decisions

Recorded 2026-11-06 (Asia/Shanghai) for operator Ryan and the dedicated agent 小龙虾 · RedClawDesktop.

A decision is attributed to Ryan when this agent's brief or an operator comment on the repository says so. This file does not invent quotations. Comment text is paraphrased; the comment URL is the record.

Product intent: [SPEC.md](SPEC.md). Dated status: [PROGRESS.md](PROGRESS.md).

## D1 — Direct-first free P2P; TURN is optional and user-provided

The product prefers a direct P2P path. Public DHT is rendezvous. ICE/STUN is the connection check. TURN is a user-supplied compatibility path, not a RedClaw-hosted default and not a requirement to ship.

Rationale already in the tree: a path with no reachable rendezvous or relay cannot succeed on every NAT, and the no-project-server design refuses a RedClaw-owned signaling server or a default relay dependency. See [solution architecture](docs/architecture/solution-architecture.md) (hard constraints and the 2026-04-02 signaling update) and Direct-first in root [AGENTS.md](AGENTS.md).

Ryan's brief for this agent restates that choice. The operator comment on PR #2 (2026-11-06, login `70565912`) says the cellular ICE failure used no TURN, by product choice, and that free P2P is preferred. That failure does not authorize a product-owned TURN service.

## D2 — Android Controller spike stays sealed pending LAN or another free direct path

Draft PR #2 (https://github.com/70565912/RedClawDesktop/pull/2) stays unmerged. S0.1 (scaffold and debug APK) is recorded as done by the operator comment. S0.2 reached DHT answer exchange and `ice_connecting` on a cellular path, then ICE failed with no media. Resume later on the same LAN or another free direct path. Do not merge until S0.2–S0.5 pass. Do not continue the spike until Ryan asks.

Source: operator comment on PR #2, 2026-11-06 00:56 UTC, login `70565912`, and Ryan's brief. The spike is absent from `main` at `40b80cc`.

This decision seals that native Controller spike. It does not, by itself, cancel or approve the separate Flutter companion plan. That distinction is an open question in [SPEC.md](SPEC.md).

## D3 — HEVC is not the default until capability, forced IDR, and tests land in phases

Superseded on 2026-11-11 by [D10](#d10--hevc-is-preferred-when-both-peers-have-hardware-hevc-h264-stays-the-fallback). Kept for history.

Do not switch the default codec to HEVC now. H.264 stays what the Host sends. Optional HEVC, if Ryan later accepts it, waits on a phased change: separate capability probes, a Controller decoder report before the Host may choose HEVC, forced IDR on every HEVC backend, and tests. A default of `auto` waits on measured LAN and remote results. The sealed Android spike is not the place to start that work.

Source: Ryan's brief, and the recommendation in open PR #3 (https://github.com/70565912/RedClawDesktop/pull/3), which is documentation only and is not merged. On `main`, `src/main.cpp` assigns `EncoderCodec::kH264` in the three Host codec sites named in [PROGRESS.md](PROGRESS.md). Kanban `M02-T01` marked done means the code paths exist; it does not mean HEVC is selected or tested. PR #3's risk table beyond those two checks was not re-run for this record.

PR #3 also leaves bitrate-versus-quality, hardware coverage, and default software x265 undecided. Those stay open.

## D4 — WCT and image_linux_rfevm are out of scope for this agent

Ryan's brief for 小龙虾 · RedClawDesktop places WCT and `image_linux_rfevm` outside this agent's work. A search of `main` at `40b80cc` found neither name. This agent does not investigate, document, or change them. What they refer to outside this repository is unknown here.

## D5 — Root charter, branch and pull request, scriptable acceptance, fresh reviewers

For this operator and this agent:

- [SPEC.md](SPEC.md), [PROGRESS.md](PROGRESS.md), and [DECISIONS.md](DECISIONS.md) are the charter and the dated snapshot. Chat history is not project state.
- [PROJECT_STATE.md](docs/runtime/PROJECT_STATE.md) and [MODULE_KANBAN.md](docs/runtime/MODULE_KANBAN.md) stay the long-form stage log and the module task ledger. Cite them. Do not copy them into the root files. On the items covered in [PROGRESS.md](PROGRESS.md), the root files win until Ryan edits them.
- Implementation lands on a branch and a pull request. This agent does not merge unless Ryan asks.
- Acceptance that can be scripted should be scripted. Existing gates are [test-matrix.md](docs/testing/test-matrix.md), the P0 runbook, and `scripts/security/check_debug_credentials.py`. An unrun manual case is not a pass.
- Reviewers are invited when needed. They are not assumed to share a previous development session. A review should be possible from the diff, these three files, and the docs they link.

Source: Ryan's brief for this agent.

## D6 — Root AGENTS.md is the agent onboarding gate

On 2026-11-06 Ryan approved, via 小程, a short gate at the top of root [AGENTS.md](AGENTS.md). Before any code or process change, an agent must read [SPEC.md](SPEC.md), [PROGRESS.md](PROGRESS.md), and [DECISIONS.md](DECISIONS.md) and follow them. The seven discipline points (single resident owner, on-demand reviewers, those three files as the only status source, scriptable acceptance, branch plus pull request, WCT / `image_linux_rfevm` out of scope, free direct P2P before TURN) are stated there. The longer build, security, and protocol rules already in that file stay below the gate.

This approval adds the gate to the open pull request. It is not a merge. Merge still needs Ryan's explicit yes.

## D7 — Capability-5 clipboard requests always end visibly

Made 2026-11-06 for the Controller-to-Host paste fix. The behavior is on main as `c8ec2a2` (PR #5).

- Shortcut paste is input the user just typed in the authorized remote view. It does not need a transfer gate. It still needs the unchanged eligibility revision: same input lease, focus, and ordinary desktop. It is refused while a transfer gate is held. `paste_verified_clipboard` keeps its gate requirement for the older paste-after-transfer modes.
- A side that cannot serve a demand or an invoke replies with `kCancel` and an error code. Silence is not allowed. Examples are a stale token, a busy slot, or no native port.
- A text or image reservation the peer has not answered within 15 s is released. That also ends a blocked native `GetData`. After the peer answers, the old rule stands: only completion, cancel, or session loss ends it. File flows keep explicit-cancel semantics, because enumeration and confirmation can take long.
- A dead native clipboard port is restarted with a 1 s to 30 s backoff instead of staying off for the session.
- Clipboard diagnostics carry only events, short id prefixes, format bits, sequence numbers, and error codes. They never carry payload contents or paths.
- No wire change. Old peers ignore the new behavior; the existing fields carry the cancels.

## D8 — Reader-specific agent rules and on-demand Codex CLI reviews

On 2026-11-06 Ryan decided, via 小程 · 工程专家, that collaboration rules are split by reader. This refines the later reader-specific part of [D5](#d5--root-charter-branch-and-pull-request-scriptable-acceptance-fresh-reviewers) and [D6](#d6--root-agentsmd-is-the-agent-onboarding-gate): who the lead is, and how a review is invited. D6 remains the historical record that Ryan approved an onboarding gate at the top of root [AGENTS.md](AGENTS.md). That gate landed on `main` through PR #4 as `eca4b31`.

Checked on `main` at `3eec53b4` (tag `v0.1.12`): the repository root has [AGENTS.md](AGENTS.md). `AGENTS.codex.md` and `AGENTS.grok.md` are not in that tree. This branch does not add them. What follows is how Ryan wants each reader instructed.

Where each reader is instructed:

- `AGENTS.codex.md` is for Codex and Cursor.
- `AGENTS.grok.md` is for Grok Bot.
- [AGENTS.md](AGENTS.md) keeps only pointers and the shared engineering constraints.

Codex and Cursor:

- A Codex or Cursor session has only its own current session. It must not seek, wait for, or hand work to 小龙虾, and it must not apply Grok Bot teammate or handoff rules.
- When Ryan names Codex or Cursor as lead, that session's reader is the current lead.

Grok Bot:

- The default resident lead remains 小龙虾 · RedClawDesktop.
- When the reader is Grok Bot, development and scriptable acceptance stay with 小龙虾 unless Ryan named someone else.

Every reader:

- Only one lead at a time.
- There are no standing review bots and no standing test bots. A milestone review, a pull-request review, or a review after acceptance goes to Codex CLI in a session separate from development. The reviewer only finds issues and must not change code. Development and scriptable acceptance stay with the current lead.
- The Codex CLI invocation already known for those reviews is `D:\DevTools\npm-global\codex.cmd`, model `gpt-6.1-sol`. The lead supplies a branch or a commit, plus the acceptance criteria in [SPEC.md](SPEC.md). 小程 may run the review.

## D9 — Show connection status and write runtime logs to files

On 2026-11-10 Ryan asked to remove the runtime log display. Connection pages and the workspace show connection progress, actionable errors, and controls. The existing file logger records diagnostics and applies its usual redaction and rotation. Explicit Debug log requests remain available to diagnostic tools.

## D10 — HEVC is preferred when both peers have hardware HEVC; H.264 stays the fallback

Ryan decided on 2026-11-11 that the PR #3 work is to be implemented and that the product should move to HEVC to save bitrate. His words: "#3需要实现，改用HEVC节约码率". The goal is lower bitrate at comparable picture quality, not higher quality at the same bitrate. This supersedes D3 and answers SPEC open question 2.

What that means in the implementation:

- Default `stream_video_codec=auto`. The Host picks HEVC only when its HEVC hardware encoder (NVENC, QSV, or AMF) is present and the Controller reports a hardware (D3D11VA) HEVC decoder. Otherwise it sends H.264. Every decision is logged as `Runtime video codec negotiated ... codec=... reason=...`.
- The Controller reports decode capabilities in optional `StreamControlMessageV1` fields 92–94. A peer that omits them is an older build and gets H.264, so mixed versions keep connecting in both directions. An older Host ignores the fields and keeps sending H.264.
- Automatic mode never uses software x265. Software x265 runs only with an explicit `hevc` preference, as a diagnostic. If an HEVC hardware encoder fails to open, the Host records that for the process and uses H.264.
- If a reconnecting Controller cannot decode HEVC, the Host rebuilds its encoder as H.264. The reverse also holds (added 2026-11-12): when a peer report that makes HEVC eligible arrives after the encoder started as H.264 (after the wait for the report, or on a reconnect that kept the pipeline), the Host rebuilds the encoder as HEVC. It does this at most once per negotiation change and not within 2 s of the last encoder start. The rebuilt encoder starts on an IDR. The log line is `Runtime video codec renegotiation ... from=h264 to=hevc reason=...`.
- Requested HEVC keyframes must be IDR pictures (forced IDR on NVENC, QSV, AMF, and x265).
- HEVC bitrate targets and floors are 70% of the H.264 budget for the same pixels and FPS. The 70% applies after the H.264 budget is clamped to its 20,000 kbps ceiling, so HEVC tops out at 14,000 kbps. 70% is an unmeasured planning value. [HEVC bitrate measurement](docs/testing/hevc-bitrate-measurement.md) is the procedure that confirms or adjusts it on Ryan's machines.
- A Controller whose decoder keeps falling behind on HEVC while the network is clean withdraws HEVC, with at most one later hardware trial under the policy amendment below. The Host then renegotiates to H.264 and logs `reason=controller_hevc_decode_overload`. This note was added on 2026-11-11 after Ryan's laptop retest. The thresholds (5 ring overruns in 30 s, no loss in the last 5 s) are planning values and have not been measured. "Loss" means frames lost on the network. Frames the Controller itself discards while waiting for the IDR after a ring gap or busy drop do not count (2026-11-12).
- HEVC hardware decode is probed only on the adapter that owns the playback window's monitor, at session start. The GUI passes that adapter to the runtime with `--present-adapter-luid` (0 means the primary monitor, for a Controller started without the GUI). If that adapter can hardware-decode HEVC, it is advertised and used. If it cannot, no other adapter is probed and HEVC is omitted from the decode report, including the software decoder, so the Host sends H.264 with `reason=peer_no_hevc_decoder`. The Controller probe log says `hevc_decode_reason=display_adapter_no_hevc`. A CPU copy from another GPU is not used. Moving the window onto a monitor owned by a different adapter does not re-probe or renegotiate during a session; restart the session. At each session start the presenter checks its window's adapter again and, if it changed, recreates its device before the probe (2026-11-12). This replaces the same-day cross-adapter rule (Ryan, 2026-11-11).
- Decode stays on the display adapter for the whole session (2026-11-12). If the shared-texture hand-over to the presenter fails, the decoder keeps the display adapter's device and copies frames to the CPU, or decodes in software. It never opens a QSV, CUDA, DXVA2 or default D3D11VA device, which could sit on another GPU. If the decode device cannot be created on the display adapter at session start, the Controller advertises H.264 only.
- Shared decode textures (2026-11-12): a published slot is not overwritten until the presenter has released that frame. With all three slots held, the new frame is dropped for display only. Playback pauses decoding and presentation together under the amendment below.
- Policy amendment (Ryan's local implementation brief, 2026-11-12): D3D11 display-off, hidden, minimized and fully occluded playback pauses both decoding and presentation. Explicit transitions, initial synchronization, native-window re-registration and a 250 ms GUI visibility/occlusion timer operate without decoded frames. Only a successful visible DXGI test clears occlusion; hide/minimize/display-off reasons must all clear. Raster Qt/OpenGL explicitly handle hide/minimize; their display-power and full-occlusion detection remain outside this amendment's backend coverage.
- The GUI acknowledges/discards through one captured ring latest sequence while paused, without payload copies, dependency scans or decode. Local shared layout v5 carries a synchronized typed session/revision/state/reason/boundary snapshot. Pause discards and stale outputs have separate counters. Runtime writing and real network accounting continue. Pause and resume resynchronization clear overrun episodes/history without undoing a latched fallback or erasing real loss history; stalled draining cannot trigger overload while ineligible.
- Resume first discards retained work, establishes a sequence boundary and invalidates decoder references. The worker resets the decoder once when accepting a current codec/geometry IDR. The 500 ms coalescer sends one initial `presentation_resumed_<trigger>` request; existing retries retain their bounded 500–2000 ms delay until displayable acknowledgement. Pausing cancels queued GUI work and suspends runtime retries. Successful IDR submission permits dependencies before first output. Session/revision fences reject in-flight output and stale requests across rapid toggles/reconnects. Audio and input authorization are independent.
- Optional feedback-validity fields 96–100 (version 1, paused, revision, local session, sample-valid) exclude paused, recovering and transition-spanning intervals from an updated Host's decode-capacity controller. Network/reassembly counts remain truthful; discarded frames never become decoded frames. Missing fields retain legacy behavior. An older Host may temporarily reduce FPS, potentially to 1, and gradually recover after wake; mixed-version on-device acceptance must record that behavior.
- HEVC retry is a trial, using monotonic time: 120 s continuously visible with fresh feedback and real output progress, no overruns, decode failures or recovery breaks; at least 5 min after fallback. A pause/transition or stale progress resets eligibility. One automatic attempt per runtime also guarantees at least 5 min between attempts. Restore the original eligible display-adapter hardware report only while the current backend is healthy and forced-H.264 is off; a software fallback prevents automatic recovery. Explicitly rearm the monitor. Observe 30 s starting with actual HEVC recovery output; publication alone or an older Host staying H.264 is not success. A second overload withdraws HEVC for the remainder of the runtime. Defaults remain unmeasured.
- Item 4.4 is deferred: only the typed local snapshot and runtime integration point exist. No Host pause, throttle, expiry or sender-pacing command ships here, so an old Controller cannot leave a new Host paused.
- Rollback and diagnosis: `--stream-video-codec h264`, profile key `stream_video_codec=h264`, or the GUI "Video codec" selector forces H.264 on the side that sets it. A Controller set to `h264` stops advertising HEVC.

Source: Ryan's 2026-11-11 instruction relayed in this agent's task brief. The assessment it accepts is PR #3 (https://github.com/70565912/RedClawDesktop/pull/3).

## Not decided in this file

- Whether the Flutter companion plan is paused along with the native spike ([SPEC.md](SPEC.md) open question 1).
- Whether to merge the PR #3 assessment document itself. The HEVC direction is decided in D10.
- Whether the 70% HEVC budget stays, after the measurement in D10.
- Licensing for shipping HEVC encode/decode in the Windows package (PR #3 risk R11). Hardware encoders and the OS decoder are used; software x265 is diagnostic only.
- Whether machine code `RC7TST01` is still a debug Host to use if the spike resumes.
- Starting the README next-phase backlog (cross-site coverage, unattended install, code signing, GUI / Agent scheduling).
