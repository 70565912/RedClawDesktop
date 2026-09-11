# RedClaw libdatachannel overlay

Pinned to the existing vcpkg 0.19.4 port (MPL-2.0 upstream), with its original
packaging patches. `vcpkg-configuration.json` selects this port and its paired
[libjuice diagnostic overlay](../libjuice/README.md), not unrelated overlays. Use the normal `build.ps1` configure path after pull,
not `-SkipConfigure`, so both configurations receive the patched native library.

`bounded-dtls-handshake.patch` changes the OpenSSL backend used by this Windows
build: bounded flight retry intervals 1/2/4/8/12 seconds, a non-renewable 120-second
steady-clock handshake lifetime and one pending timer per transport. OpenSSL's
own retry-count guard is retained; the 12-second cap avoids exhausting it before
the absolute deadline. Scheduler wakes are capped at four seconds, including for
a passive peer with no flight timer, and do not imply a retransmission. Timer wakes
use the existing receive scheduler rather than decrementing its pending count
without a matching enqueue. There are no extra media queues or threads.

Certificate/fingerprint verification, ciphers, ICE credentials, DTLS role selection,
and established-session recovery are unchanged. GnuTLS/mbedTLS are not covered by
this timing patch. No shared SDK source is edited. The dependency source archive
is still pinned and SHA512-verified by the port.

Regression: `DhtSignaling/DelayedAnswerTransport` covers Answer delivery after
0/22/45/90 seconds and an Answer never delivered (bounded failure at 120 seconds).
The 22-second case failed before the patch with native DTLS timeout at ~31 seconds.
Public-DHT GUI acceptance remains a separate gate from these native transport tests.

`typed-ice-check-diagnostics.patch` forwards a synchronous typed libjuice callback
through `PeerConnection::onIceCheck`. The wrapper consumes it under its existing
generation/lifetime guard; no application callback, native call, log formatting,
or payload handling runs from that callback. Only fixed event kinds, pair indexes
and candidate types cross the API. This build pins `USE_NICE=OFF`; ICE-TCP is not
supported, regardless of the requested configuration. Both `datachannel.dll` and
`juice.dll` must be deployed together with the new executable: the extended native
ABI is not compatible with an old DLL. This patch does not alter ICE timers,
credentials, retry policy, media buffering or packet routing.

The separate `bounded-initial-ice-checks.patch` forwards
`Configuration::initialIceCheckWindowMs` to libjuice. RedClaw DHT enables 120 s;
non-DHT callers retain zero/native defaults. This closes the initial-ICE expiry
boundary that the DTLS patch alone cannot fix when outbound-dependent filtering
prevents any checks from succeeding before the Answer arrives. Reconfigure and
deploy both native DLLs with the executable; do not mix the extended ABI with old
libraries. Physical no-response paths still need paired diagnosis and may remain
unusable without a relay; this timing change is not a NAT bypass.

`ignored-udp-errors.patch` appends three ignored UDP error kinds and an observation
availability kind, preserving numeric values of older categories. Deploy EXE and
both rebuilt DLLs together after a full configure. The wrapper's separate
candidate evidence uses HMAC-SHA256 keyed by the candidate owner's private ICE
password, truncated to 128 bits, over canonical address/port/UDP/MID/type. The
emitting side and applying side can compare identities for the same Offer/Answer
generation without logging the address, port or key. No public-salt address hash
or credential is published. The token changes when ICE credentials change.
Observations are bounded to 64 per native generation and kept separately from
the 64-event recent diagnostic ring. Observation timestamps are local steady
time, not comparable clocks or a NAT allocation-age measurement. Candidate
identity agreement is not proof of actual wire egress or remote socket liveness.

References: [upstream source](https://github.com/paullouisageneau/libdatachannel/tree/v0.19.4)
and [OpenSSL timer API](https://docs.openssl.org/3.6/man3/DTLS_set_timer_cb/).
