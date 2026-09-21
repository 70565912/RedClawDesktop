# Local workspace control v1

Implementation contract for X00-T21; current task status is on the
[module kanban](../runtime/MODULE_KANBAN.md).

The Controller optionally listens on the current-user named pipe
`RedClawDesktop.WorkspaceControl.v1`. Start the candidate GUI with
`--enable-workspace-control`; use `--workspace-control-name <name>` for an
isolated instance. There is no HTTP service or additional public port.
The listener checks the connecting process's user identity. The existing
GUI/runtime pipes still check the expected process IDs in both directions.
The API uses the connected desktop's ordinary-user authority; it does not
grant elevation, create a second shell, or prompt for individual commands.

## Requests, retries and results

One UTF-8 JSON request and one response, each newline terminated, use version 1.
The request contains `version`, `request_id`, `operation`, `parameters`.
Call `capabilities` first; subsequent requests also carry its `instance_id`.
That identity changes when the listener is recreated. Never attach an old
unconfirmed command to a new instance or reconnect and resubmit automatically.

Mutating requests are deduplicated by ID and the canonical operation/parameters
digest. Identical retries return the original submission response; changed
parameters return `request_id_conflict`. The bounded per-instance registry
retains 4096 submissions without eviction; once full it rejects new mutations
with `request_history_full`. Reads and status queries continue. This prevents
an old, evicted command from silently executing a second time.

Asynchronous submissions return `operation_id`. Use `operation.status`,
`operation.result`, `operation.cancel`, `operations` and `events`.
Operation listings retain 128 recent records; events retain 2048 records and
return pages of at most 128. Cursors and 64-bit byte counters are decimal
strings. An expired cursor returns `gap: true` and the first available cursor.
A missing operation reports `operation_not_found`, never fabricated success.
Content belongs in explicit result reads; ordinary diagnostics contain no
command text, file bytes or clipboard payload.

The PowerShell 7 client is [invoke-workspace-control.ps1](../../scripts/service/invoke-workspace-control.ps1).
It checks pipe server ownership and never resends an unconfirmed request.
`-WaitSeconds` only polls status; expiry returns `wait_timed_out` and leaves
the operation running. Keep its operation ID and instance ID.

## Terminal

`terminal.open/input/read/status/resize/exec/cancel` share the GUI's
`TerminalCoordinator` and existing `TerminalController`. The terminal can
start without initializing a WebView or showing a task window. Output enters
a bounded 1 MiB shared journal; the GUI consumes the same bytes with its own
cursor, while remote ACKs are independent of an absent/slow view.

Terminal capability 2 adds execution and cancellation messages to schema 1.
The packaged PowerShell profile emits versioned, nonce-bound execution/output
records into a private anonymous pipe. Only the Shell receives its writer through
the process creation handle whitelist. The runtime consumes already-buffered
records without blocking its owner thread; the OS pipe provides bounded
backpressure. No new listener, Shell or network port is involved. ConPTY carries
the unchanged interactive terminal stream. Separating receipts avoids Windows
terminal repaint ordering and control-sequence loss.

Execution dot-sources the supplied script in the existing interactive scope.
The main `PSConsoleHostReadLine` callback reports readiness and completion;
prompt rendering cannot finish an operation while an input line is being
repainted. No prompt-text matching or silence timer determines completion.
The profile disables persistent PSReadLine history for this managed shell.
It also clears an inherited Windows ignore-Ctrl+C flag inside the Shell,
so cancellation works when its launcher belongs to a separate process group.
ConPTY's already buffered small writes are combined into the existing 16 KiB
wire chunk before ACK; queue capacity and lossless VT ordering stay unchanged.

`exec` requires a main-prompt receipt and no pending interactive input.
It exclusively owns input until completion; other calls receive busy. GUI
input is disabled while owned and an explicit interrupt remains available.
Cancellation sends Ctrl+C and waits for a prompt or shell exit.
Transfer gates pause new input while output/status remain readable.

Results include `powershell_success` and nullable `last_native_exit_code`.
The latter is the shell's most recent native-process exit code, which may
belong to an earlier command. It is not the exit code of every PowerShell
statement. Execution results contain the merged PowerShell/native output streams
with operation identifiers; `terminal.read` returns the original terminal VT
stream, including interactive and direct-console output. Each journal retains
1 MiB. `output_base64` preserves exact bytes; concatenate decoded pages before
decoding UTF-8 because a page can split a character. `output` is a convenience
view. Large output requires cursor paging. Missing receipts, shell exit without
completion or connection loss produce an unknown result; commands never replay.

## Files and clipboard

`TransferCoordinator` owns selection sequencing, submission and result state
for both the panel and API. `files.browse/upload/download` use the existing
runtime bridge, disk worker, 64-bit offsets, temporary files, SHA256 verification
and result journals. Upload/download accept a `sources` array containing files
or directories, `destination`, and `conflict`: `keepBoth` (default),
`overwrite`, or `skip`. Recursive traversal belongs to the existing worker.
Progress distinguishes received bytes from verified/committed bytes.
Result pages load asynchronously from the existing journal; `items_state`
reports loading, ready or failed. Directory results use a bounded 2048-entry
history with explicit gaps.

Clipboard capability 3 adds `clipboard.read/write/paste`. Read/write accept
`target: remote|local`. Both use the same transfer gate and verified snapshot
format. Local clipboard batches loop existing workers locally while the peer
holds the shared mutation gate. Read retains an exported snapshot and does not
publish to either system clipboard. Write accepts explicit `text`,
`snapshot`, PNG `image` path, `files` array or `formats` entries with
`kind` and `base64`/local `file`. Format kinds are Unicode UTF-16LE text (1),
HTML clipboard format (2), RTF (3), PNG (4), DIBV5 (5), DIB (6).
Explicit source descriptors stay owner-local and are rejected from peers.
All disk work and format preparation use the existing transfer workers.

`clipboard.paste` targets the remote current window, independently of copying
or publishing. It rechecks focus generation, session and input eligibility,
submits once and never activates another application. Focus adapters permit
deterministic tests without altering the user's clipboard or injecting keys.
`clipboard.copies.list/open/cleanup` accept `target: remote|local` (default remote)
and retain existing received-copy behavior;
cleanup accepts only existing opaque copy IDs, never arbitrary paths.
There is no continuous clipboard synchronization.

One file/clipboard batch per connection remains the shared rule. Terminal
input and new Agent mutations are blocked during that batch; media and
already-running output continue.

## Compatibility and qualification

File capability remains 1; terminal moves 1 to 2, clipboard 2 to 3. New
messages/semantics are sent only at their negotiated common version. Older
peers keep their original terminal stream, file transfer and clipboard paste.
Unsupported new calls fail explicitly. Neither connection nor acceptance
requires equal versions, commits or executable hashes.

Qualification uses isolated local fixtures, injected clipboard/focus adapters
and supported-capability direction/reconnect tests. Actual application paste
is an optional developer evaluation and is absent from the automatic sequence.
Build via `build.ps1 -NoPublish`, then publish a new independent directory
with `publish.ps1 -PublishDirectory <candidate>`. Runtime switching and
GitHub Release binary publication are separate explicitly authorized operations.
The source synchronization checkpoint includes the implementation, client,
contract and qualification summary; committing/pushing source does not switch
the running application or publish the candidate archive.

The local two-GUI harness accepts `-EnableWorkspaceControl` for its isolated
Controller endpoint. For rolling-upgrade qualification, `-HostRestartCount 1`
with `-HostRestartRuntimeExe <candidate-exe>` keeps the Controller alive and
restarts only the harness-owned Host into the specified independent candidate.
It records both executable hashes and reconnection receipts. The capability
contract remains independent of those diagnostic hashes.

The 2026-09-21 qualification included 22 automatic suites (301 cases, zero
skips), a separate runtime-options suite, injected six-format clipboard and
focus tests, and actual DHT/ICE GUI media plus 23 script-level checks. The
script checks included hidden Shell initialization, shared variables, native
exit-code semantics, deduplication, wait/cancel, bounded output gaps and a
64 MiB file/directory round trip with matching SHA256 and empty directories.
Actual application paste remains optional and was not performed.

That qualification and the independent candidate archive precede the merge of
the Host media updates through `2d9fbca`. The archive retains its original
manifest and hash. Merged-source build/regression results are recorded
separately in the development log; source synchronization does not replace
the archive or establish new media-performance evidence.

Final-candidate mixed-version checks covered both directions against the
previous Debug bundle. The new Controller observed common terminal/file/
clipboard capabilities 1/1/2, used the legacy Shell and remote directory
browse, and explicitly rejected new execution/clipboard semantics (eight
script checks). In the reverse direction the old Controller stayed running
while its owned old Host restarted into the candidate over online DHT/ICE;
real media resumed after instance adoption. Reconnect-without-command-replay
is also covered by the native terminal integration test.

A local concurrent-load observation recorded baseline/load/recovery display
rates of 31.14/15.40/7.03 FPS, maximum Controller heartbeat intervals of
43.00/65.22/36.10 ms, and four-process private-memory peaks of
448.3/519.5/456.2 MiB. Terminal history stayed at or below 1 MiB; sampled Host
media active/pending depth was zero. The scene was not fixed, and these are
observations, not a claim of unchanged media performance or a repair of
X00-T20. Preserve the frame-rate reduction for follow-up investigation.

## Calling examples

```powershell
$api = '.\maintenance\invoke-workspace-control.ps1'
& $api -Operation capabilities
& $api -Operation terminal.open -WaitSeconds 10
& $api -Operation terminal.status
& $api -Operation terminal.exec -Parameters @{command='$value=41; $value'} -WaitSeconds 10
& $api -Operation operation.result -Parameters @{operation_id='<returned-id>'}
& $api -Operation files.upload -Parameters @{
  sources=@('C:\work\report.txt','C:\work\logs')
  destination='D:\incoming'; conflict='keepBoth'
}
& $api -Operation clipboard.read -Parameters @{target='remote'}
& $api -Operation clipboard.write -Parameters @{target='remote';text='diagnostic note'}
& $api -Operation clipboard.paste
```
