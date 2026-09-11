# RedClaw libjuice diagnostic overlay

Based on vcpkg libjuice 1.3.1 with its original source SHA512 and packaging patch.
The optional `juice_config_t::cb_check` synchronous callback retains eleven original
check categories (request/response TX/RX, validation rejection, unmatched response,
timeout, nomination TX/RX, send failure and missing remote credentials), plus a
pair index and candidate types. It never carries addresses, credentials, SDP,
packet bytes or raw error text. No new threads, timers, queues or logging handler.

Only peer connectivity-check entries emit TX/success RX events; STUN discovery
and TURN allocation are excluded. RX means accepted by validation and dispatch,
not raw UDP reception. Validation rejection before a pair is known uses pair -1.
Nomination TX/RX reports the USE-CANDIDATE check, not proof of successful final
selection. Local candidate type 0 is retained when libjuice has no explicit local
pair candidate; never guess a type. The callback must not reenter the ICE agent.

Build with the paired libdatachannel overlay and deploy both DLLs together. Raw
libjuice debug/error output can include sensitive authentication details; do not
enable it for user evidence. Tests use the production wrapper through a loopback
UDP permission filter; they are not a full NAT implementation or physical-link
acceptance.

`receive-boundary-diagnostics.patch` appends fixed receive and mapping categories.
`socket_rx` counts datagrams dispatched by the native connection reader before
selection, and STUN datagrams after selection. It is **not NIC packet capture or
an all-media byte counter**. Known peer/server/unknown-source counters partition
that set before STUN parsing; a peer-reflexive source can initially be unknown.
Parsed peer-shaped Binding messages are counted before integrity verification;
`peer_stun_parsed` does not certify authentication. STUN server responses remain
separate from accepted peer checks. State filtering, non-STUN preselection,
parser errors and unknown-address dispatch each have distinct counters.

Candidate consistency is evaluated locally: host candidates must match addresses
obtained from the gathering socket; check send destinations must match their
resolved remote candidate. Only match/mismatch, peer generation and pair IDs are
exported. This is not proof of NAT reachability or wire-SDP delivery, and local
candidate type 0 still means unknown. `socket_io_failed` covers creation/poll/read
connection failures, not a claim about a particular OS firewall rule.

The wrapper aggregates new categories with at most one diagnostic ring/text event
per category per second. Counters retain all occurrences; established non-STUN
media does not emit these callbacks. No global native verbose logger is enabled.
Deploy EXE and both rebuilt native DLLs together; older diagnostic consumers do
not know the appended enum values, although media/control wire schemas are unchanged.

`ignored-udp-errors.patch` observes `ECONNRESET`, `ENETRESET` and
`ECONNREFUSED` before `udp_recvfrom` discards them. They retain the existing
non-terminal semantics and do not masquerade as validation failures or accepted
peer ingress. On Windows a reset can report an earlier ICMP error; these counters
do not identify which check/server caused it, or prove a firewall/NAT cause.
POLL/THREAD sockets expose `udp_error_observation_ready=1`; shared MUX attribution
is unavailable, not a zero-error claim. The production wrapper uses POLL. One
receive call yields after at most 64 ignored errors. Only pre-selection errors
emit the optional callback; there is no new packet queue or verbose native logger.

`bounded-initial-ice-checks.patch` adds an opt-in initial connectivity window
(`initial_check_window_ms`, zero preserves upstream behavior, hard cap 120000).
The first pending peer check starts a monotonic deadline that trickle never
renews. The normal fast burst is retained; exhausted initial checks continue
once per two seconds per pair using the existing entries/timer. At the deadline
they fail instead of resetting their retry count. Send errors are not retried by
this extension. It does not change STUN discovery, TURN allocation, authenticated
selection or established-session consent. A no-response DHT attempt can therefore
last longer, but stays bounded; a fast successful path incurs no added wait.
