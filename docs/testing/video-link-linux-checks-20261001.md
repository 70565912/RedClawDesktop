# T29–T31 bounded review and Linux test handoff (2026-10-01)

## Scope and evidence

The operator authorizes Linux minimal joint checks and one further bounded
T30/T31 investigation, followed by temporary closure if historical attribution
remains unavailable. The prior T27 source-based acceptance is unchanged.

Evidence reference: `t29-t31-local-20261001-01`. This turn changes test code and
repairs capture-module portability; no serving application is built, replaced,
restarted or reconfigured. No desktop scene, network load or recurring task runs.
Full source/test-binary/ZIP hashes, build logs and all three rounds of XML remain
local. These are test-source identities, not an identity claim about a running Host.

The [test-only harness](../../tests/latency_minimal/README.md) compiles production
protocol/capture/render and sender logic. It excludes GUI, helper/service startup,
ICE sockets and the main executable. Vendor-description logic is made portable;
the D3D11 adapter check stays inside `_WIN32`. Windows behavior is unchanged.
Existing codec assertions are moved to a common header, preserving the original
software/NVENC test callers. Three new cases exercise sender timing provenance.

## Local production-code results

Test-only MSVC Debug build succeeds. Three complete rounds each pass all three
CTest groups: **9 CTest executions, 21 GoogleTest case executions, zero failures,
zero disabled/skipped cases**. T29 encodes and decodes **60 frames** in total.

| Check | Established result | Limit |
| --- | --- | --- |
| T29 | Four sessions per round, requested 30/5/1/5 FPS, sizes 1184x666/1778x1000/1184x666/1184x666; nominal 30 FPS, time base 1/30, GOP 60 and geometry budgets 1892/4267 kbps hold; monotonic PTS, actual packets, decoded geometry and no rate-update attempt pass | Fixed synthetic codec input; no desktop or cross-LAN performance claim |
| T30 | ACK, external expiry, stale-snapshot negative control and scheduled expiry are distinguished; multiple expiries and concurrent budget/reset cases pass | Original frame 14176 remains historically unattributed |
| T31 | Predicate-mutex hold inflates elapsed wait; transport-state and send-callback injections land in their own trace buckets, with zero in-flight wait and overflow | Does not reproduce or explain the original 6.215 s successful-send gap |

Three-round medians [range], in milliseconds:

- T30 missing-refresh negative control: 1015.726 [1015.685, 1016.142], expected
  deadline drop; autonomous refresh: 9.645 [9.400, 10.331], successful send.
- T31 requested 20 ms wait with an 80 ms mutex hold: 87.179 [80.742, 92.179].
- T31 80 ms transport-state injection: 91.423 [91.340, 92.008]; send-callback
  injection: 92.762 [90.627, 93.272]. These deliberately delayed tests are not a
  latency benchmark or a measured optimization percentage.

The first conventional test build cannot regenerate because SDK package-manager
writes are unavailable. The separate test-only project then reuses installed
libraries without package installation. A missing protoc discovery is corrected
in the generated build configuration; the harness now fails early if protoc is
not available. No permissions or persistent system settings are changed.

## One further attribution pass

The current source confirms that `MediaPacerWait` measures through reacquisition
of its predicate mutex. Native Windows waits unlock before waiting and lock again
on return; the Linux condition-variable path similarly reacquires the lock. Thus
large recorded overshoot can include scheduling and mutex contention.

The production transport-state callback includes the channel snapshot/native
bufferedAmount/availableAmount/maxMessageSize queries and lifecycle validation,
then acquires the runtime callback mutex to publish the stats snapshot. Existing
`transport_state_us` covers this whole callback region; it does not split these
locks or native calls. The callback runs outside the pacer mutex, so its elapsed
region does not prove a pacer-mutex deadlock. The new bounded injections validate
this distinction without changing transport policy.

No original raw X30 recording is present in this local task's X00-T27 evidence
folder; the historical review uses the retained sanitized repository report.
That report lacks thread-scheduler and individual lock acquisition traces. The
original feedback window has rotated. Neither current source nor this round can
uniquely assign the old T30 stall or T31 gap to scheduling, mutex contention or a
native callback. **Historical causes remain unknown.** No speculative bitrate,
queue, priority or network adjustment is made.

## Linux joint-check delivery

The operator selects the existing cloud chat **设置 RedClawDesktop**. A single
bounded task is sent there with a ZIP containing tracked-source changes, new
cases/harness and a full SHA256 manifest. Base revision is `58002bb`; ZIP hash12
is `e27282187672`. The executor owns its legal methods and environment checks.
Three repetitions of the same production-code checks are requested, with raw
records retained by that executor and only sanitized outcomes returned.

The Linux executor confirms ZIP SHA256/CRC and all 10 manifest files. Three
**T29 rounds pass: 3 CTest executions, 3 GoogleTest executions, 12 sessions and
60 real software H.264 encoded/decoded frames, zero failures/skips**. The full
specified codec contract passes. Reference `ev-d80657bbeb1d45629e2717499bb2d34f`.
This is the selected executor's result, independently reported from Linux.

T30/T31 initially execute zero rounds because four production `std::min/max`
expressions mix `unsigned long long` with LP64 `uint64_t` (`unsigned long`).
Affected functions are receiver-feedback assessment, congestion drain and pacer
queue/deadline calculations. Four explicit `std::uint64_t` template arguments
repair compilation without changing values or policy. The incremental patch is
`71bb1139918c`; full SHA256 and normalized-LF source hashes remain in the delivery
manifest. A new local test-only build and six CTest / 18 GoogleTest executions
(three rounds of only T30/T31) pass, zero failures/skips.

The same Linux executor verifies the incremental patch and all three normalized-LF
source hashes, then completes **three rounds each of T30/T31: 6 CTest executions,
18 GoogleTest case executions, zero failures/skips**. T29 is not repeated. Both
callback injections have `in_flight_wait_us=0` and `overflow=0`; the opposite bucket
is 1 us. Negative-control deadline drop, successful autonomous send, no fabricated
ACK/loss, multiple expiry and concurrent budget/reset assertions pass.
Reference `ev-9c416aa2d42c40d3960bb14105ea8551`.

| Linux controlled-test timing, ms | Three-run median | Range |
| --- | ---: | ---: |
| T30 missing-refresh negative control | 1015.145 | 1015.141–1015.183 |
| T30 autonomous expiry refresh | 9.950 | 9.445–9.995 |
| T31 20 ms wait with 80 ms predicate-mutex hold | 80.350 | 80.235–80.426 |
| T31 transport-state injection bucket | 80.094 | 80.090–80.106 |
| T31 send injection bucket | 80.088 | 80.080–80.132 |

Together the Linux checks complete 9 CTest / 21 GoogleTest executions and 60
software codec frames. All test programs have ended; `blocked_stage=null`. No
second executor or recurring monitor is created. This is the executor's sanitized
receipt; its raw records remain there. Timing differences between the Windows and
Linux checks are not treated as optimization gains or cross-machine latency.

## Closure disposition

**T29 supplemental Linux acceptance is complete. T30/T31 are temporarily closed
under the operator's explicit instruction after this bounded round.** The repair
and instrumentation mechanisms pass on both test platforms; the historical T30
stall and T31 6.215-second gap remain **unknown**, with `incident_complete=false`.
The LP64 changes repair Linux compilation and supply no evidence about the old
Windows stall's root cause. No whole-system/end-to-end tail target or historical
incident resolution is claimed. Any renewed incident investigation needs new
attributable scheduler/individual-lock/native-call evidence; no follow-up load or
recurring monitoring is started by this closure.

## References

- [T29/T27 optimization scope](video-link-optimization-x00-t27.md)
- [Retained sender incidents and repairs](video-send-attribution-20260929.md)
- [Authoritative task status](../runtime/MODULE_KANBAN.md)
