# Independent runtime-directory upgrade

Updated: 2026-09-16

This is the fixed upgrade path for an authorized two-machine maintenance task. It preserves each GUI's role and arguments and updates the complete runtime directory. Task status and remaining release gates are in [PROJECT_STATE](../runtime/PROJECT_STATE.md).

Actual peer maintenance is a separately authorized operation, not a required manual release test. Automated qualification uses isolated local runtime fixtures; developers decide whether to perform real deployments. The safety/ownership checks below still apply whenever maintenance is explicitly requested.

## Prepare and hand off

1. Protect pre-existing work, synchronize to the agreed exact commit and run `build.ps1 -Configuration Debug -NoPublish` plus relevant tests. Publish the complete bundle to a separate candidate directory with `publish.ps1 -Configuration Debug -PublishDirectory <candidate>`.
2. Identify the existing GUI and its runtime children by PID, executable path, current-user ownership, interactive session and process start time. Unrelated GUI instances sharing the formal executable are an ambiguity, not targets to stop.
3. Invoke the following launcher with the observed GUI PID and local evidence directory. Gate switches attest completed checks; they do not run those checks.

```powershell
.\scripts\service\start-agent-runtime-upgrade.ps1 `
  -TargetPid <gui-pid> -CandidateDirectory <candidate> `
  -ExpectedGitSha <full-commit-sha> -EvidenceDirectory <local-evidence> `
  -BuildGatePassed -FocusedTestGatePassed
```

`-PlanOnly` validates and writes a reviewable owner-only plan without registering a task or stopping a process. Keep plans private: preserved launch arguments may contain local connection data.

4. The launcher verifies complete EXE/DLL/Qt-plugin/protocol-tool manifests and starts a current-user, interactive, non-elevated, one-shot Task Scheduler worker through `start-agent-lifecycle-supervisor.ps1`. There is no remote arbitrary-command queue.
5. The independent worker reports validation, copy and probe stages while preparing the pending full directory. Each advancing stage has a bounded wait; repeated or unknown stages cannot keep an operation pending indefinitely. After verifying the candidate command entry it announces `handoff_ready`. The launcher verifies independent worker identity and acknowledges the exact plan hash. Only then may the worker close the existing GUI/runtime.
6. The worker verifies file release, renames the original full directory into a rollback sibling, installs the full candidate, relaunches the GUI with preserved arguments and checks startup. For a target that owned a runtime, startup also requires the replacement runtime. The startup allowance is 60 seconds to cover measured cold initialization; this does not change the GUI/media heartbeat limit. Receipt `completed` means startup passed; network connection and real video are separate gates.

Every stop must follow a verified `independent_worker_owns_upgrade` handoff. Do not stop the Host/Agent first and leave a command that depends on its lifetime at `restart_pending`.

## Failure behavior

Wrong process identity, a changed repository commit, ambiguous runtime ownership or corrupt candidate files reject the operation. A file-release failure leaves the original directory intact and restarts the stopped original GUI. If the installed candidate fails startup, the entire old directory and arguments are restored. Failed candidates and rollback directories remain available as local evidence. A DHT/ICE connection failure alone does not trigger repeated rollback.

The full bundle file-release check waits briefly for WebView2 and other owned components to finish normal teardown after GUI/runtime exit. Persistent occupation still fails the gate; unrelated browsers are never stopped to force a swap.

The repository plan and receipt use explicit `redclaw.runtime-upgrade.*.v1` schemas. The portable terminal path uses a version-2 plan with an immutable GUI-owned session context; see the [remote workspace contract](../architecture/remote-workspace-v013.md). Both launchers use the same bounded preflight wait and verify the worker's executable, user, session and exact arguments. A failed handoff cancels only its own unacknowledged task and keeps the target running.

Every directory move is checked against explicit sibling paths. A failed recovery still writes a final receipt with its cause; if graceful close is refused, it retains the process and directories rather than forcing a swap. Reading an unfinished receipt whose worker has exited reports `interrupted` without rewriting the original receipt or replaying the operation. Inspect the receipt and local error before issuing a new explicit request.

## Validation

```powershell
.\scripts\service\test-runtime-directory-upgrade.ps1
.\scripts\service\test-runtime-directory-upgrade.ps1 -Scenarios parent_exit
.\scripts\service\test-runtime-directory-upgrade.ps1 -PortableMaintenance
.\scripts\service\test-runtime-directory-upgrade.ps1 -PortableMaintenance -Scenarios delayed_runtime,delayed_window,recovery_refused
```

The isolated fixtures cover identity mismatch, bundle corruption, file occupation, startup rollback and parent exit. The parent-exit case attaches the initiating process to a `KILL_ON_JOB_CLOSE` job, closes that job after handoff, and verifies that the independently launched new GUI survives worker exit. Fixtures use separate directories and do not connect to a peer.

For real rolling upgrades, upgrade the peer Host first while the local Controller waits. Confirm the Host's identity and fresh capture/encode/send plus Controller decode/presentation increments, then upgrade the Controller and reconnect. Record each endpoint's own commit and hashes, roles, channel readiness, zero synthetic frames and decode/presentation failures, screenshots and remaining issues. Supported mixed versions negotiate common capabilities; identical binaries are not a connection requirement.
