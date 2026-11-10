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

Made 2026-11-06 for the Controller-to-Host paste fix (branch `cursor/fix-controller-host-clipboard-paste-0a34`).

- Shortcut paste is input the user just typed in the authorized remote view. It does not need a transfer gate. It still needs the unchanged eligibility revision: same input lease, focus, and ordinary desktop. It is refused while a transfer gate is held. `paste_verified_clipboard` keeps its gate requirement for the older paste-after-transfer modes.
- A side that cannot serve a demand or an invoke replies with `kCancel` and an error code. Silence is not allowed. Examples are a stale token, a busy slot, or no native port.
- A text or image reservation the peer has not answered within 15 s is released. That also ends a blocked native `GetData`. After the peer answers, the old rule stands: only completion, cancel, or session loss ends it. File flows keep explicit-cancel semantics, because enumeration and confirmation can take long.
- A dead native clipboard port is restarted with a 1 s to 30 s backoff instead of staying off for the session.
- Clipboard diagnostics carry only events, short id prefixes, format bits, sequence numbers, and error codes. They never carry payload contents or paths.
- No wire change. Old peers ignore the new behavior; the existing fields carry the cancels.

## D9 — Show connection status and write runtime logs to files

On 2026-11-10 Ryan asked to remove the runtime log display. Connection pages and the workspace show connection progress, actionable errors, and controls. The existing file logger records diagnostics and applies its usual redaction and rotation. Explicit Debug log requests remain available to diagnostic tools.

## Not decided in this file

- Whether the Flutter companion plan is paused along with the native spike ([SPEC.md](SPEC.md) open question 1).
- Whether to merge PR #3 after review, and the HEVC product questions PR #3 leaves open.
- Whether machine code `RC7TST01` is still a debug Host to use if the spike resumes.
- Starting the README next-phase backlog (cross-site coverage, unattended install, code signing, GUI / Agent scheduling).
