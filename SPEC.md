# Product specification

Audience: operator Ryan and the dedicated agent 小龙虾 · RedClawDesktop.

Status date: 2026-11-06 (Asia/Shanghai). Surveyed `main` at `40b80cc`. Codec items updated 2026-11-12 for D10 and the local playback-pause amendment.

This file is the product-intent source for that pair. It cites documents already in the repo and does not add requirements. Unknowns are marked. Publishing this file does not change runtime behavior.

## How these files fit together

- Product intent and acceptance: this file.
- Dated progress and next actions: [PROGRESS.md](PROGRESS.md).
- Decisions that constrain the agent: [DECISIONS.md](DECISIONS.md).
- Agent onboarding gate: [AGENTS.md](AGENTS.md). Read this file, PROGRESS, and DECISIONS before any code or process change.
- Shipped description for users: [README.md](README.md) and [README.en.md](README.en.md).
- Long-form stage log and module ledger: [PROJECT_STATE.md](docs/runtime/PROJECT_STATE.md) and [MODULE_KANBAN.md](docs/runtime/MODULE_KANBAN.md).

Those runtime boards were not rewritten for this snapshot. Where they disagree with [PROGRESS.md](PROGRESS.md) or [DECISIONS.md](DECISIONS.md) on the overlay-log merge, the sealed Android spike, or HEVC, the root files win until Ryan edits them. Module rows on the kanban remain the ledger for work that is already on `main`.

## Product

RedClawDesktop is a Windows-first P2P remote desktop and remote Agent workspace. A developer connects to their own Windows development machine by machine code, sees the real desktop, sends explicitly authorized keyboard and mouse input, and works with AI coding Agents on the Host. The normal product flow does not ask for a peer IP address.

Online public DHT is the rendezvous. ICE/STUN negotiates the path and prefers a direct P2P link. TURN is optional and user-provided. There is no RedClaw-owned public signaling server or default relay.

Sources: [README.md](README.md), [solution architecture](docs/architecture/solution-architecture.md) (product goal, hard constraints, 2026-04-02 signaling update), root [AGENTS.md](AGENTS.md) (Direct-first).

## In scope on current main

Product version metadata is 0.1.13. The [v0.1.13 release notes](docs/releases/v0.1.13.md) describe the Windows x64 Developer Preview, and the [GitHub releases](https://github.com/70565912/RedClawDesktop/releases) list what is available to download. The user-facing capability list is the table in [README.md](README.md). The acceptance-critical subset is:

1. Windows x64 Host and Controller in one desktop GUI: local machine code, peer machine code, connection password, wait/connect, and a playback area. The GUI shows connection progress and errors. Runtime diagnostics are written to local log files.
2. Connection password required. Passwordless v0.1.3 and earlier builds are rejected. See [connection password](docs/architecture/connection-password-v1.md).
3. Public-DHT rendezvous and ICE/STUN direct connect. TURN can be configured by the user. It is not a product dependency. See the ICE section of [README.md](README.md) and [network exit selection](docs/architecture/network-exit-selection-analysis.md).

Debug Host launchers accept an ICE UDP port per machine. An optional ignored
`local/debug-host-ice-port.txt` supplies the port, while an explicit launcher
argument takes precedence. Without either, the existing supervisor default applies.
4. Real desktop capture (Windows Graphics Capture / Desktop Duplication) and a real encode, transport, decode, and present path. Codec selection follows [D10](DECISIONS.md#d10--hevc-is-preferred-when-both-peers-have-hardware-hevc-h264-stays-the-fallback). The Host sends HEVC when both endpoints have the required hardware support. Other combinations, including older peers that omit HEVC negotiation, use H.264. The setting `stream_video_codec=h264` forces H.264.
5. Authorized keyboard and mouse. Missing capability, lease, or lifecycle channel fails closed. Secure-desktop ambiguity refuses or pauses input. An accepted display switch preserves active control during the bounded wait for the new display. Input resumes after confirmation and presentation of the new geometry. The ordinary-desktop recovery prompt keeps remote input responsive and lets the user dismiss the prompt, switch windows, or ask the Host to close the window that triggered the prompt. See [ordinary desktop input](docs/architecture/ordinary-desktop-remote-input-v1.md).
6. Control, Media, and Agent channels. Codex and Cursor providers run on the Host inside registered projects. See [remote Agent bridge](docs/architecture/remote-development-agent-bridge-v1.md).
7. Capabilities already described for v0.1.12, enabled only by the common negotiated set: paste-time clipboard, confirmed file transfer, remote system audio, embedded terminal, directory upgrade, and Host update-restart protection. A connected Controller shows paste failures over playback. The Connection overlay handles reconnect status while File/Clipboard keeps the interrupted result and its current visibility. See [D7](DECISIONS.md#d7--capability-5-clipboard-requests-always-end-visibly).

   Repeated shortcuts for the same local copy retain its offer identity. Native and keyboard paste requests share one reservation; a rejected native request cannot overwrite an active shortcut installation or continuously re-advertise the clipboard while that reservation is held.

   A text/image shortcut authorizes the demand, and the Host submits paste only after verified content and local clipboard readiness, with the original focus/input authorization. Files retain explicit confirmation. An early background clipboard read cannot act as the shortcut's completion receipt.

   Native copies with image and incidental file representations use image paste unless they explicitly request a file move; file-only copies and cuts retain the file confirmation path. The snapshot must provide every advertised image format, without including unconfirmed files. PNG native aliases share the existing wire format.
8. Cross-version interoperability. DHT, session, media, control, Agent, and Debug Bridge protocols are versioned. Still-supported releases connect through the common capability set. The same Git commit, product version, or binary is not a connection requirement. See root [AGENTS.md](AGENTS.md) and the protocol contract in the solution architecture.

## Controller playback pause amendment (PR #10 local follow-up)

The D10 amendment pauses decode as well as presentation and keeps the shared ring drained. Resume keeps the last presented picture visible while waiting for a fresh post-boundary IDR and worker-owned reference reset, with coalesced recovery and bounded-delay retries. This keeps a settled desktop visible even when an older Host has no new picture to send. Pause/resume intervals are excluded from overload and capable-Host decode-capacity assessment. HEVC has at most one conservative hardware trial after fallback. Display-adapter-only probing, audio, remote input and input authorization retain their existing contracts. Sender pause/throttle (4.4) remains deferred. Backend limits and the trial policy are recorded in [D10](DECISIONS.md#d10--hevc-is-preferred-when-both-peers-have-hardware-hevc-h264-stays-the-fallback); [PR #10 acceptance checklist](docs/testing/hevc-bitrate-measurement.md#pr-10-playback-pause-acceptance-checklist) tracks the unrun device checks.

Accepted limits of the D3D11 coverage poll: shaped windows with holes can overcount coverage, and non-opaque, color-keyed, or per-pixel-alpha layered covers can undercount it. A non-layered `WS_EX_TRANSPARENT` window that still paints an opaque rectangle is not detected as a full cover.

## Acceptance already defined in the repo

This list points at existing gates. It does not create new ones.

- Desktop UI is the acceptance surface: machine codes, wait/connect, and visible Controller playback. Users are not asked to type an IP. Root [AGENTS.md](AGENTS.md) priority list (2026-05-10) and [P0 desktop-stream runbook](docs/testing/p0-dual-machine-desktop-stream-runbook.md).
- The product path uses online rendezvous and ICE. Direct IP/TCP, file signaling, and local dual-process smoke are diagnostics. A CLI pair that reaches `cli_pair_connected_ui_playback_required` is not the P0-DS-07 pass; Controller UI playback evidence is.
- Opening the GUI is idle: it does not wait, connect or capture. Explicit Host wait and authenticated Controller connect use real desktop video by default, including GUI supervisor/worker starts without `--stream-smoke`. `--no-desktop-video` disables local desktop video for diagnostics; set it on both endpoints for a video-free pair. It does not change the session protocol or permissions.
- `--stream-smoke` requests timed transport/render assertions; `--stream-require-capture` additionally checks that a real Host desktop frame entered the path. Smoke, preview-only or RGB packets are not proof of real capture or visible GUI playback (root [AGENTS.md](AGENTS.md)).
- Timed Controller smoke requires a local presentation from the supervised GUI or a completed preview write from a headless worker. Packet acceptance, decode without output and submission to a display pipe are not presentations. Restoring a Host must preserve its previous video policy, including a legacy flagless opt-out.
- A queued GUI repaint, shader initialization failure or preview file read does not count. File-backed playback must reject a retired session's frames even if diagnostic output was lost.
- A Windows Debug Controller can start idle with an explicit private GUI test profile. The profile has its own INI, process logs, coordination journal and current-user control name. The one-machine presentation gate requires two advancing painted frame IDs from the same worker and playback generation with regular password authentication. The presenter must leave foreground ownership and the user's window order undisturbed. Its D3D11 canvas may present while covered by another window. Hidden, minimized, cloaked and display-off states still pause playback.
- Private startup must not load the user's Qt logging rules. Neither private worker may read or replace the interactive clipboard. An accepted positive run or signaling timeout must reach the worker unchanged; zero alone means unlimited.
- The private presenter must first appear behind the foreground application and remain ineligible for activation when that application closes or minimizes. A reconnect hint must leave the user's foreground application unobscured.
- Release automation follows [test-matrix.md](docs/testing/test-matrix.md). Only unattended cases are release gates. Physical keys, visual judgment, UAC, and special hardware or topology are optional developer evaluation. An unrun case is not a pass.
- Ordinary builds, candidate updates and PRs use focused, change-dependent automated tests per [D11](DECISIONS.md#d11--merges-are-gated-on-local-buildtests-github-build-and-test-is-advisory). Full CTest requires an explicit request or a separately agreed release acceptance.
- GitHub-hosted virtual machines are not the real-desktop acceptance environment ([GitHub Actions validation](docs/testing/github-actions-validation.md)).
- Security fail-closed rules in root [AGENTS.md](AGENTS.md) stay in force.
- Independent reviews use a read-only subtask, as specified by [D8](DECISIONS.md#d8--reader-specific-agent-rules-and-on-demand-subtask-reviews) and D11.

## Out of scope for this agent

- The Android Controller spike. It is not on `main`. Draft PR #2 stays sealed. Do not merge it and do not continue S0.2–S0.5 until Ryan resumes it on a LAN or another free direct path. Details: [PROGRESS.md](PROGRESS.md), [DECISIONS.md](DECISIONS.md).
- WCT and `image_linux_rfevm`. Ryan's brief for this agent places both outside its scope. A search of `main` at `40b80cc` found no matches. What those names mean outside this repo is unknown here.
- macOS portable and web mini clients. Planning-only unless explicitly re-scoped ([clients/AGENTS.md](clients/AGENTS.md), solution architecture portable platform scope).
- Architecture non-goals: a full IDE on mobile or tablet, enterprise IAM/SSO, a multi-tenant cloud control plane, and autonomous AI-to-AI relays that hide raw results (solution architecture, "Non-Goals for MVP").
- A RedClaw-owned public signaling service or a default hosted TURN server (solution architecture hard constraints).

## Open questions for Ryan

1. Portable client versus the sealed spike. The solution architecture still lists an Android phone/tablet companion as current implementation scope: Flutter, short sessions, status, logs, and guarded commands, without default keyboard and mouse control ([clients/AGENTS.md](clients/AGENTS.md)). Kanban `M11-T01` is `planned` on `main`. Draft PR #2 is a different artifact: a native Android Controller (not the Flutter companion) aimed at connect, decode, and later touch input. The seal covers that spike. Whether the Flutter companion plan is also paused is not stated.
2. Answered 2026-11-11 by [D10](DECISIONS.md#d10--hevc-is-preferred-when-both-peers-have-hardware-hevc-h264-stays-the-fallback): lower bitrate at comparable quality, HEVC preferred with H.264 fallback, software x265 not used automatically. Still open: whether the 70% HEVC budget holds after [the measurement](docs/testing/hevc-bitrate-measurement.md), and HEVC licensing for the shipped package.
3. README's next phase (wider cross-site and TURN coverage, unattended install and upgrade, code signing, GUI and Agent scheduling cost) is still product backlog. `M07-T02` (signed unattended installer) is `planned`. This charter does not start that work.
4. README states that historical T30/T31 stall causes remain unknown after a bounded review, and that strict end-to-end latency targets are not claimed passed. Nothing in this survey closes them.

## Unknowns checked against the tree

- Debug Host machine code `RC7TST01` appears in PR #2 and does not appear on `main`. Whether it is still the optional resume target is unconfirmed. Passwords for it are not in git and must not be added. See [PROGRESS.md](PROGRESS.md).
- Field visual acceptance of the overlay-log merge is not recorded in-tree. Windows CI on PR #1 did pass `build-and-test`. See [PROGRESS.md](PROGRESS.md).
