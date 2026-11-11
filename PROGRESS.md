# Progress

As of 2026-11-06 (Asia/Shanghai).

`main` includes the clipboard paste fix at `c8ec2a2` (PR #5). Product version metadata is 0.1.12 so the Windows publish script can run after this preparation merges. That script has not been run. The published GitHub package remains v0.1.11, and there is no v0.1.12 tag.

Intent and acceptance: [SPEC.md](SPEC.md). Decisions: [DECISIONS.md](DECISIONS.md). Agent gate: [AGENTS.md](AGENTS.md).

## 2026-11-10 — Runtime logs go to files (M10-T02)

- Remove the runtime log card and its copy and clear actions from the connection pages. Remove the diagnostic log displays from the workspace. Connection progress and actionable errors remain visible. The existing diagnostic writer still records GUI and runtime messages in the rotating local files. See [D9](DECISIONS.md#d9--show-connection-status-and-write-runtime-logs-to-files).
- The Debug application built through `build.ps1 -SkipConfigure -Target redclaw_desktop -NoPublish`. An isolated GUI run returned home and exited successfully. The home and Host waiting snapshots show the connection controls with no log card, and the GUI diagnostic marker appears in the file log. Evidence is under `build/reports/ui-file-log-20261110`.
- With a workspace-local test temporary directory, all 42 focused connection, workspace, and diagnostic writer cases passed. The D3D11, file logger, and structured redaction CTest suites also passed. The broader UI run in the default environment hit filesystem and Windows credential or registry restrictions. The running Host was not replaced.

- The independent Debug candidate passed a 345-file manifest check and a restricted-PATH startup probe. CIM could not verify the running Host from the build environment, so the maintenance worker did not take ownership. The existing Host continued DHT publication in standby.

## Clipboard paste fix, on main

Squash-merged as `c8ec2a2` (PR #5), from `ce0990fd`. Decision: [D7](DECISIONS.md#d7--capability-5-clipboard-requests-always-end-visibly). [Release notes](docs/releases/v0.1.12.md).

- Root cause. With capability 5, the Host injects the fixed Ctrl+V right after it installs the offer, before any bulk transfer exists. Injection went through `RemoteInputSession::paste_verified_clipboard`, which only works while a transfer gate is held. So every Host shortcut paste failed with `clipboard_input_revoked`. The Controller only showed the error in the hidden file panel. That is why no four-key paste inject reached the Host.
- Contributing liveness defects, all silent before:
  - A stopped native port (listener failure, queue overflow, exception) stayed dead for the whole session.
  - Clipboard capture and `OleSetClipboard` were not retried while another program held the clipboard open, so the Host kept a stale `RedClaw.ClipboardOffer.v5`.
  - A Host demand with a stale token, or one that hit a busy slot, was dropped without a reply.
  - Unanswered reservations and shortcut waits had no bound.
- Fix:
  - `paste_shortcut_clipboard` injects the shortcut paste. It is bound to the eligibility revision and refused while a transfer gate is held.
  - Every request the session cannot serve gets a `kCancel` with an error code.
  - A text or image reservation with no peer reply is released after 15 s, which ends a blocked `GetData`.
  - A shortcut capture that does not answer fails after 5 s.
  - Failed offer sends are retried.
  - A dead native port restarts with a 1 s to 30 s backoff.
  - The native port retries capture, install, and listener registration.
  - The Controller UI shows every refusal.
  - Runtime lines starting `Runtime clipboard role=` and the GUI line `Clipboard shortcut intercepted` log state changes, never payloads.
- Automated: `redclaw_clipboard_demand_liveness_tests` (11 cases) and `RemoteInputSessionTests.ShortcutClipboardPasteNeedsNoTransferGateButStaysRevisionBound`. Both run in the unit CI. On Linux with a capability shim, all 11 liveness cases passed on the fix, and 10 failed against the previous session code. PR #5 CI jobs `build-and-test`, `hint`, and `docs-guard` passed (Actions runs [42169935054](https://github.com/70565912/RedClawDesktop/actions/runs/42169935054), [42169935102](https://github.com/70565912/RedClawDesktop/actions/runs/42169935102), and [42169935053](https://github.com/70565912/RedClawDesktop/actions/runs/42169935053)). Ryan confirmed bidirectional Controller-to-Host clipboard paste on live two-machine Debug builds of `ce0990fd`, now on main as `c8ec2a2`. The Windows Release portable package and its publish-step smoke are produced later and are not part of this preparation.
- Run locally: `cmake --build --preset debug-local --target redclaw_clipboard_demand_liveness_tests redclaw_remote_input_session_tests`, then `ctest --test-dir build/vs2022-x64 -C Debug --output-on-failure -R "clipboard_demand_liveness|remote_input_session"`.
- Procedure written with the fix:
  1. Copy text on the Controller. The Host log shows `event=offer_received`, and then `event=port offer_installed`.
  2. Focus a Host text field, take input control, and press Ctrl+V, right Ctrl+V, and Shift+Insert. The text appears on the Host. The Host log shows `event=paste_inject ... result=ok`.
  3. Use Host right-click paste. The text appears, or the app returns within 15 s.
  4. Any failure is shown in the Controller file/clipboard panel with a reason.
- Rollback: revert `c8ec2a2`, or redeploy the previous Debug binaries on both machines. The wire messages are unchanged, so mixed old and new builds still connect. Shortcut paste needs the new Host build. With an old Host it still fails as before, but a new Controller times out and shows the failure.

## Mainline

- PR #4 (`eca4b31`) is merged: root `SPEC.md`, `PROGRESS.md`, `DECISIONS.md`, and the root `AGENTS.md` onboarding gate.

- `40b80cc` is the merge of PR #1, "Remove the Connection overlay log and clean logs on startup" (https://github.com/70565912/RedClawDesktop/pull/1). Author Ryan Chen. Merged 2026-11-05 14:46 UTC (2026-11-05 22:46 Asia/Shanghai).
- The floating Connection panel no longer shows the scrolling runtime log. Progress, pairing, handshake, live link, path and picture status, and reconnect/disconnect stay. The connection page keeps its own log. Diagnostics stay in capacity-limited files. The same behavior is described in [README.md](README.md) and the 2026-11-05 section of [PROJECT_STATE.md](docs/runtime/PROJECT_STATE.md).
- PR #1 checks that completed before merge: `docs-guard` success, `hint` success, `build-and-test` success (Actions run `42042846320`). The PR body says the authoring environment had no VS2022 toolchain. No new dual-machine visual acceptance for this merge is written in the tree.
- The last published package is the v0.1.11 Developer Preview ([release notes](docs/releases/v0.1.11.md)). Version metadata and [v0.1.12 release notes](docs/releases/v0.1.12.md) are prepared. `main` since that tag includes the dark workspace, the connection overlay, overlay-log removal (PR #1, `40b80cc`), the root status files (PR #4, `eca4b31`), and the clipboard paste repair (`c8ec2a2`).
- The Android spike is not on `main`. Searches for `android-controller-spike` and `M11-S0` found nothing. Kanban `M11-T01` remains `planned`.
- On `main`, the Host selects H.264 in `src/main.cpp` at the startup/resize profile (`preferred_codec`), the bridge request (`codec`), and the adaptive profile (`preferred_codec`), all `EncoderCodec::kH264`. `EncoderCodec::kHevc` mapping exists. A search of `src/` found no `video_decode` capability field. That matches the "Host sends H.264" and "no decoder capability" claims in PR #3. The rest of that PR's risk table was not re-measured here.

## Open pull requests

Do not merge either pull request as part of this snapshot.

### PR #2, draft — Android Controller spike

https://github.com/70565912/RedClawDesktop/pull/2

- Draft, open, not merged. Head `d3b2a046303c6ae2ff2329e96b622a581f81d619` on `cursor/android-controller-spike-s0-1-9958`. Base recorded as `main` @ `40b80cc`. GitHub `mergeable_state` at survey time: `dirty`.
- S0.1, as described by the PR: portable `redclaw_controller_core` plus an arm64 JNI / MediaCodec shell. Not a Host and not the Flutter companion. Terminal, Agent, and transfer are out of that spike's scope.
- Operator comment on 2026-11-06 00:56 UTC (08:56 Asia/Shanghai), GitHub login `70565912`: pause the spike and return to `main`. S0.1 scaffold and the debug APK (`android-spike-s0.1` prerelease) are OK. S0.2 on a cellular path reached DHT `answer_published` / Host `answer_applied` and `ice_connecting`, then ICE failed (`phase=failed`, no media). No TURN, by product choice (prefer free P2P). Resume later on the same LAN or another free direct path. Do not merge until S0.2–S0.5 pass. The branch is kept and the PR was converted to draft.
- The only check run returned for this head was `windows-portable-package`, conclusion failure (Actions run `42061689311`, about 11 seconds). The cause was not investigated for this snapshot.
- Codex review on the PR did not complete (usage limit).

### PR #3, open — HEVC assessment, documentation only

https://github.com/70565912/RedClawDesktop/pull/3

- Open, not a draft, not merged. Head `4f437ae19ab6f56bfa8127af8aefdfc9b1767f90` on `cursor/hevc-migration-assessment-6a79`. Base `main` @ `40b80cc`. GitHub `mergeable_state` at survey time: `unstable`.
- The PR adds `docs/architecture/hevc-migration-assessment.md` and index, kanban, and devlog links. It states that it does not change encoder, decoder, protocol, or runtime behavior. That file is not on `main`.
- Recommendation in the PR body: do not switch the default to HEVC. Optional HEVC, in phases, with H.264 remaining the default and the fallback. A Controller decoder-capability report comes before the Host may choose HEVC. Forced IDR and tests come before any behavior change. Android MediaCodec on the sealed spike is left untouched.
- No HEVC encode or decode run and no bitrate or latency measurement were made for this snapshot. Numbers in the PR are that PR's hypotheses.
- At survey time, `docs-guard` and `hint` had succeeded and `build-and-test` was still in progress (Actions run `42141441966`). Codex review did not complete (usage limit).

## Optional debug Host

PR #2 names Host machine code `RC7TST01` as the LAN target for S0.2. That string is not in the `main` tree. The 2026-11-06 seal comment does not repeat it. It describes the cellular ICE failure.

Treat `RC7TST01` as optional context for a future LAN resume, not as a current `main` task. This survey did not contact any Host. Whether that machine is still in Debug, still publishing, or still the resume target is for Ryan to confirm. Do not add its password or a local debug config to git.

## Next steps

Items 1–5 remain open. Item 6 is the publish step after this preparation is on `main`.

1. Leave PR #2 draft and sealed. Do not merge, rebase, or continue Android connect, decode, or input work until Ryan asks for a same-LAN or other free-P2P resume.
2. Leave PR #3 open for review. If Ryan accepts the recommendation, the first implementation step is still that PR's P0: separate H.264 and HEVC capability probes, forced IDR on each HEVC backend, and HEVC tests, with no default change. Do not start that implementation until asked.
3. Keep the Windows product on direct-first P2P. TURN stays optional and user-provided. Do not add a product-owned relay to paper over the cellular ICE failure.
4. The next phase named in [README.md](README.md) — wider cross-site and TURN coverage, unattended install and upgrade, code signing, and lower GUI / Agent scheduling cost — stays backlog. It is not the next edit unless Ryan assigns it.
5. Ryan confirms the open questions in [SPEC.md](SPEC.md): Flutter companion versus the sealed spike, the HEVC product goal, and whether `RC7TST01` is still relevant.
6. From a clean Windows checkout of `main`, run `scripts/release/publish-github-release.ps1 -Version 0.1.12`. That step creates the tag and the GitHub Pre-release. This preparation does not run it.
