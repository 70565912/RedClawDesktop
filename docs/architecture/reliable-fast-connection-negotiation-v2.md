# Reliable DHT Connection Negotiation v2

Decision date: 2026-08-22

## Goal and Boundary

RedClawDesktop uses the public Mainline DHT as its product signaling path. It does not require a RedClaw-owned HTTPS rendezvous server. DHT exchanges only encrypted signaling snapshots; desktop media still travels peer-to-peer through ICE/libdatachannel.

The protocol must remain correct when either endpoint starts late, a DHT write takes tens of seconds to become visible, snapshots are duplicated, or an unconnected Controller process is restarted. Slower discovery may extend the visible connecting phase, but it must not make Host and Controller silently move through incompatible ICE attempts.

## Root Cause of the Missed Windows

The earlier flow incorrectly mixed three identities:

- the fixed eight-character connection code;
- one Controller connection request;
- one Host-created ICE generation.

The connection code was reused correctly, but both endpoints also rebuilt ICE from local timers and process-lifetime counters. Public DHT is a latest-state register with eventual propagation, so an old offer, a new answer, and candidates from another local rebuild could be observed at different times. Each item was individually valid, but the pair did not describe the same ICE generation. Retrying the same timing rules reproduced the mismatch instead of converging.

A second lockout existed after introducing request-driven offers: once a Host accepted one Controller request tag, it rejected a different tag forever. If the old Controller exited before connecting, a new Controller process could publish indefinitely while the Host remained bound to the abandoned request.

These are connection-state-machine defects, not evidence that Host and Controller need different fixed codes.

## Identity Model

- `session_code`: stable discovery locator and encryption input. Both roles intentionally use the same code, for example `RC7TST01`.
- `connection_request_tag`: random tag created by one Controller Connect runtime. It distinguishes a live Controller request from an abandoned one.
- `generation`: monotonically increasing ICE generation owned only by Host.
- `offer_description_tag`: digest of the Host offer for the generation.
- `answer_description_tag`: digest of the Controller answer bound to that offer.
- `acknowledged_answer_tag`: Host proof that it applied the matching answer.
- `revision`: monotonically increasing version of one role's cumulative DHT snapshot.

The role lanes are separate DHT mutable records. The Host lane always contains the newest cumulative Host state; the Controller lane always contains the newest cumulative Controller state. They are not queues of transient messages.

## Connection Flow

```text
Controller                         Public DHT                         Host
    |                                  |                               |
    |-- request(tag=C, generation=0) ->|                               |
    |   refresh same request ---------->|-- latest Controller state --->|
    |                                  |                               | create fresh ICE
    |                                  |<-- offer(C, generation=G) -----|
    |<-- latest Host offer/candidates -|    refresh same generation     |
    |                                  |                               |
    | apply matching offer             |                               |
    |-- answer(C,G,offer_tag) -------->|                               |
    |   refresh answer/candidates ---->|-- latest Controller answer --->|
    |                                  |                               | apply answer
    |                                  |<-- ACK(C,G,answer_tag) --------|
    |<-- latest Host state + ACK ------|                               |
    |                                  |                               |
    |================ matching ICE generation G =======================|
    |====================== data channel open =========================|
```

### 1. Host standby

Host joins DHT and polls the Controller lane, but remains at generation zero. It does not create an offer, gather candidates, or start an unanswered-offer rotation merely because it has waited for a long time.

### 2. Controller request

One Connect action creates a new random request tag and publishes a generation-zero `request` snapshot. Until a matching Host offer is fetched, Controller refreshes the same cumulative request with a new revision and expiry. A delayed Host therefore observes the current request instead of depending on a short-lived launch overlap.

### 3. Fresh Host generation

After Host accepts the request tag, Host creates fresh ICE credentials and advances the generation once. Every offer and candidate snapshot for this attempt carries the same request tag, generation, and offer tag. Republish and keep-alive operations update the revision/expiry but do not advance the generation.

### 4. Bound Controller answer

Controller accepts only an offer that echoes its active request tag. It follows a higher Host generation, never creates one. The answer echoes the request tag, Host generation, and exact offer tag. Controller keeps the Host candidates cached until its matching answer and at least one local candidate have been published successfully. It then releases the cached Host candidates immediately, so public-DHT ACK latency does not consume libjuice's finite connectivity-check window.

### 5. Host acknowledgement

Host accepts only the answer matching its current request, generation, and offer tag. After applying it, Host first publishes a size-bounded direct ACK record, then republishes its cumulative snapshot with the same acknowledged answer tag and candidates. Controller uses this ACK as correlation evidence and for failure classification, but not as the candidate-release gate. Host may establish the same correlated generation earlier through inbound checks after it receives the Controller candidates; duplicate ACKs and snapshots are idempotent.

The libtorrent adapter may expose the highest-sequence signature-verified mutable result before the DHT traversal's final `authoritative=true` callback. Libtorrent has already verified the BEP44 signature before this callback; the operation stays pending so a later higher sequence or authoritative empty result can replace or clear the interim value. This removes the otherwise unavoidable full traversal tail from each request/offer/answer read without weakening signature validation.

### 6. Connection or classified retry

An ICE failure before answer correlation and candidate completion is treated as an incomplete signaling attempt. It does not terminate the runtime or let Controller invent a generation. Only Host may create the next generation, and only after the matching answer/candidate exchange is complete and the attempt has actually failed.

## New Controller Supersession

The product assumes one Controller lease per Host. While no session is connected, a different authenticated request tag in a newer Controller-lane snapshot supersedes the previous request:

1. Host clears the abandoned request's offer, answer, ACK, and per-generation candidate state.
2. Host keeps the process-lifetime DHT diagnostics but resets the failed-attempt budget.
3. Host creates a fresh higher generation bound to the new request tag.
4. The new Controller rejects any still-propagating offer that carries the old request tag.

Once connected, Host does not replace the active Controller merely because another request appears. That request is rejected as busy/conflicting until the active session ends.

This rule prevents an unattended Host from being permanently locked to a Controller process that has already exited.

## Non-Negotiable Invariants

1. A wall-clock or delivery timeout may trigger republish, diagnostics, or a user-visible timeout; it never advances generation by itself.
2. Only Host creates an offer and advances generation.
3. Controller accepts only a Host offer matching its active request tag.
4. Host accepts only an answer matching request tag, generation, and offer tag.
5. All DHT snapshots are cumulative and safe to observe repeatedly.
6. A lower generation or mismatched tag is rejected without applying SDP or candidates.
7. Process-lifetime publish/fetch counters are diagnostics only; current-generation decisions use typed negotiation state.
8. The fixed connection code is never used as the ICE generation identity.
9. Long Host standby has no active ICE expiry window.
10. A newer Controller request can replace only an unconnected attempt.
11. Controller releases cached Host candidates only after its matching answer and at least one local candidate are published; ACK is correlation evidence, not an ICE-start timer.
12. An early mutable DHT result is usable only after libtorrent signature verification, while the authoritative traversal remains alive.

## Runtime States

Host:

`idle -> request_received -> offer_ready -> answer_applied -> ice_connecting -> connected`

Controller:

`idle -> request_ready -> request_published -> offer_applied -> answer_ready -> answer_published -> answer_applied -> ice_connecting -> connected`

The GUI maps these typed states to the inline Start-a-Session status. The runtime stays active during a pre-ack DHT delay or recoverable ICE failure; the user must not press Connect repeatedly to create overlapping processes.

## Timeout and Refresh Rules

- Host standby: unlimited when GUI signal timeout is zero; no offer rotation.
- Controller request: refresh the same request every DHT keep-alive interval.
- Offer/answer/candidates: republish the same cumulative generation snapshot until connected or superseded.
- DHT delivery delay: remain in the same negotiation state and expose the age/revision in logs.
- Pre-correlation ICE failure: keep signaling alive and wait for the current exchange to complete.
- Post-correlation ICE failure: Host may advance to a new generation after the configured patient delay.
- New Controller request: replace the unconnected attempt immediately; do not wait for the abandoned generation's failure timer.
- User cancellation: stop the runtime explicitly. It must not leave an invisible background connection attempt.

## Acceptance Criteria

- A Host can wait at least 600 seconds at generation zero and then accept a Controller request.
- Delayed and duplicate snapshots do not change generation.
- A restarted Controller with a new request tag replaces an unconnected old attempt and receives a fresh Host generation.
- A competing request cannot replace an already connected Controller.
- A Controller never accepts an offer for another request tag.
- An answer never applies to a different Host generation or offer tag.
- GUI quick Wait/Connect actions always select DHT and use no signaling timeout by default.
- A same-SHA two-machine run reaches matching request/generation tags, `connected=true`, `channel_open=true`, real Host capture/transmit counters, nonzero Controller receive/decode/render counters, and visible desktop playback.

## Product Boundary

DHT removes the need for a RedClaw-owned signaling server, but it cannot create a media route through every NAT/firewall combination. ICE still needs at least one usable direct IPv4/IPv6, mapped, srflx, or user-provided relay path. This network limitation must be reported separately from negotiation mismatch; it must not be hidden by silently starting unrelated generations.
