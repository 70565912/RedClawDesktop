# Progress

As of 2026-11-06 (Asia/Shanghai).

Surveyed local `main` at `40b80ccbea9e6a14b8fffddcfc1d793b449a5af2` after `git fetch origin main`. `HEAD` and `origin/main` match. Open pull requests were read from GitHub and were not modified. This snapshot does not start the next steps below.

Intent and acceptance: [SPEC.md](SPEC.md). Decisions: [DECISIONS.md](DECISIONS.md).

## Mainline

- `main` @ `40b80cc` is the merge of PR #1, "Remove the Connection overlay log and clean logs on startup" (https://github.com/70565912/RedClawDesktop/pull/1). Author Ryan Chen. Merged 2026-11-05 14:46 UTC (2026-11-05 22:46 Asia/Shanghai).
- The floating Connection panel no longer shows the scrolling runtime log. Progress, pairing, handshake, live link, path and picture status, and reconnect/disconnect stay. The connection page keeps its own log. Diagnostics stay in capacity-limited files. The same behavior is described in [README.md](README.md) and the 2026-11-05 section of [PROJECT_STATE.md](docs/runtime/PROJECT_STATE.md).
- PR #1 checks that completed before merge: `docs-guard` success, `hint` success, `build-and-test` success (Actions run `42042846320`). The PR body says the authoring environment had no VS2022 toolchain. No new dual-machine visual acceptance for this merge is written in the tree.
- The published package is still the v0.1.11 Developer Preview ([release notes](docs/releases/v0.1.11.md)). `main` is ahead of that tag with later UI work, including the dark workspace, the connection overlay, and this log removal. [MODULE_KANBAN.md](docs/runtime/MODULE_KANBAN.md) (Updated 2026-11-05) was not edited for this snapshot, so its narrative can lag the root files.
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

These are the actions that follow from the survey. They are not started here.

1. Leave PR #2 draft and sealed. Do not merge, rebase, or continue Android connect, decode, or input work until Ryan asks for a same-LAN or other free-P2P resume.
2. Leave PR #3 open for review. If Ryan accepts the recommendation, the first implementation step is still that PR's P0: separate H.264 and HEVC capability probes, forced IDR on each HEVC backend, and HEVC tests, with no default change. Do not start that implementation until asked.
3. Keep the Windows product on direct-first P2P. TURN stays optional and user-provided. Do not add a product-owned relay to paper over the cellular ICE failure.
4. The next phase named in [README.md](README.md) — wider cross-site and TURN coverage, unattended install and upgrade, code signing, and lower GUI / Agent scheduling cost — stays backlog. It is not the next edit unless Ryan assigns it.
5. Ryan confirms the open questions in [SPEC.md](SPEC.md): Flutter companion versus the sealed spike, the HEVC product goal, and whether `RC7TST01` is still relevant.
