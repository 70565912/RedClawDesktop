# Serverless DHT Rendezvous and Local Helper Design

Decision date: 2026-05-17

## P0-DS-05R2 publication advancement (2026-09-05)

`IDhtRendezvousStore` and `DhtRendezvousClient` now return `DhtPublishResult` with pending/succeeded/retryable/permanent states. Pending is never inferred from error strings. Client retries keep immutable ciphertext and runtime schedules pending advancement on its short loop interval, not the five-second failure retry. Indirect publication advances a round-robin cursor over at most eight chunk operations per call with a five-millisecond scheduling target; summary is published only after every chunk succeeds.

The libtorrent adapter submits puts without a per-chunk sleep. All alert readers share one cursor because native alert pointers remain valid only until the next pop. Each advancement processes at most 64 alerts within its scheduling slice. The shared `DhtPublishRegistry` bounds logical records to 256 and pending leases to 64, expires record/attempt leases, keeps completed history at most 60 seconds, and indexes completion by public key, salt and actual signed sequence. Late old signing callbacks/completions are counted but cannot restore an expired entry. The signing callback owns no runtime/store pointer and retains no extra plaintext frame data.

The default native lookup-plus-put lifetime is 120 seconds, bounded by record expiry; this is not a blocking wait or a poll interval. An eight-second operation lifetime failed the real public-DHT gate by discarding successful delayed confirmations. Production registry regressions retain the same immutable operation through 30/45/90-second completion delays. Runtime continues short-interval advancement throughout.

The repository libdatachannel 0.19.4 overlay separately fixes OpenSSL DTLS flight scheduling for delayed signaling: 1/2/4/8/12-second retries, the original native retry-count guard, one pending scheduler wake and a non-renewable 120-second steady-clock handshake lifetime. Certificate/fingerprint checks and established connections are unchanged. The real wrapper delay matrix (0/22/45/90 seconds and Answer absent) passes; public-DHT picture acceptance remains separate. Reconfigure after sync so old native DLLs are not silently reused.

These changes preserve V4 publisher-instance and request-tag rules, candidate application before store confirmation, bounded failed-generation recovery, and Controller Answer without ACK echo. Source tests do not by themselves establish public-DHT convergence or diagnose the historical established-session disconnect.

## Goal

RedClawDesktop should support a no-project-server connection mode:

- No RedClaw-owned public server is required.
- No paid third-party relay service is required for the default path.
- Two private-network clients try to connect through DHT rendezvous, ICE, STUN, IPv6, automatic port mapping, and synchronized hole punching.
- A helper service may run on either client, another LAN computer, or a NAS.
- When no reachable path exists, the runtime must fail with clear NAT diagnostics instead of pretending the mode can guarantee universal traversal.

This mode is the preferred direction for the next serverless rendezvous implementation slice. Managed rendezvous and TURN remain optional compatibility paths, not the default product assumption.

## Hard Limit

Pure serverless NAT traversal cannot guarantee success for every pair of private networks.

It fails when both sides are behind restrictive NAT/firewall conditions and neither side has any reachable path, such as:

- dual carrier-grade NAT with symmetric mappings,
- enterprise firewall blocking UDP and unsolicited TCP,
- no public IPv6,
- failed UPnP/PCP/NAT-PMP mapping,
- no user-provided relay/helper that is reachable from the other side.

The product promise for this mode is therefore: maximize direct success rate, classify failures accurately, and offer next steps.

## Runtime Modes

### 1. LAN direct

- Discover local peers through host candidates, mDNS, and optional LAN helper announcements.
- Use direct local addresses when both endpoints are on the same LAN.
- This mode should never require typing an IP address in the normal UI; diagnostics may expose discovered addresses.

### 2. DHT-backed online rendezvous

- Use public BitTorrent/Mainline DHT bootstrap nodes only to join the DHT network.
- Publish encrypted short-lived offer/answer/candidate records under a pairing-derived topic.
- Do not publish machine codes, tokens, fingerprints, LAN addresses, or user identity in plaintext.
- Do not send media through DHT.

Suggested flow:

```text
machine code + pairing secret
 -> derive rendezvous topic and encryption keys
 -> Host publishes encrypted offer + candidate hints
 -> Controller looks up topic and verifies signed payload
 -> Controller publishes encrypted answer + candidates
 -> both sides run ICE checks and synchronized UDP punching
 -> successful pair carries encrypted media/control channels
```

Implementation notes:

- Prefer BEP44-style signed mutable records when the chosen DHT library supports them.
- Keep records short-lived, normally 2 to 5 minutes.
- Include protocol version, role, nonce, monotonic sequence, expiry, and signature.
- Rate-limit publish/poll loops and cache DHT routing state between runs.
- Treat DHT as hostile: records can be missing, delayed, replayed, or poisoned.

### P0-DS-05R publication and transport ordering (2026-09-04)

Local DHT storage confirmation, remote Answer acknowledgement and transport
connection state are independent facts, not serial prerequisites:

- Apply authenticated, current-generation remote candidates as soon as the
  matching description is applied and the transport is live. Do not wait for
  local `dht_put_alert` success, complete local gathering, or Host ACK. Continue
  publishing local cumulative candidates independently; pending is not success.
- A failed/disconnected/closed transport is terminal. Initial Controller
  recovery waits only a monotonic `1/2/4/8/16` second backoff (16-second cap;
  existing 20-repair limit), then publishes a new correlated request. Missing
  old candidates, publication confirmation or ACK cannot extend that wait.
  Only the Host generates the replacement Offer. Exact failed Offers, stale
  generations and mismatched request tags retain their existing protections;
  established-session recovery still owns failures after a successful session.
- Host publishes the matching Answer ACK; Controller only observes it and
  never copies it into its outbound Answer. Pending publication content and
  revision stay stable across remote ACKs. Real local SDP/candidate changes
  still change the publication key; refresh expiry remains separate. Do not
  increase the 700-byte direct-record budget to hide ACK echo inflation.
- Late ACK/publication cannot move connected state backwards or revive a
  failed attempt. A connection may be established before either confirmation;
  each diagnostic flag must still report its actual observed fact.

Deterministic tests use the real encrypted DHT client with delayed local put
confirmation at 0/30/45/90 seconds, bounded initial recovery, stale ACK/Offer
isolation and 697-byte versus erroneous 713-byte Answer fixtures. This does
not substitute for public-DHT two-machine testing or explain a separately
observed established-session transport disconnect with unknown native cause.

### 3. Local helper service

`redclaw_connectd` may run as a lightweight helper on:

- the Host machine,
- the Controller machine,
- another always-on LAN computer,
- a NAS that can run binaries, containers, or supported app packages.

Helper responsibilities:

- keep DHT routing warm,
- maintain UPnP/PCP/NAT-PMP mappings for the LAN when allowed,
- store encrypted rendezvous artifacts in its own private local store,
- relay encrypted LAN-side traffic between local devices when needed,
- expose health and NAT diagnostics to the desktop UI.

The helper does not create magic cross-Internet reachability. It becomes a true Internet relay only when it has a public IPv6 address, an explicit router port mapping, a reachable vendor tunnel that exposes a programmable endpoint, or another user-controlled reachable path.

### 4. Local helper deployment

A supported NAS or always-on LAN computer may host the helper when it provides
a documented package, container, or service runtime. App-only remote-access
flows are outside the automatic connection design and are not used for
repository-based exchange.

## Candidate Priority

The connectivity engine should gather and try candidates in this order:

1. same-LAN host candidates,
2. helper-provided LAN candidates,
3. public IPv6 candidates,
4. explicit UPnP/PCP/NAT-PMP mapped UDP candidates,
5. STUN server-reflexive UDP candidates,
6. synchronized UDP hole-punching candidates exchanged through DHT,
7. TCP/TLS 443 candidates where supported,
8. user-provided relay/helper candidates.

TURN is still supported when the user explicitly provides it, but it is not required for the default no-project-server mode.

## Security Requirements

- Pairing material must be high entropy or stretched through the existing password/KDF policy.
- DHT rendezvous payloads must be end-to-end encrypted.
- Every published record must carry expiry, replay protection, and authenticated sender identity.
- Logs must redact pairing secrets, private keys, raw encrypted blobs, and sensitive local paths.
- A helper service must fail closed if local capability, consent, or token validation is missing.

## Implementation Milestones

Status update 2026-05-17:

- Landed the runtime/profile/UI contract for `signal_transport=dht`, `dht_bootstrap`, `enable_port_mapping`, `enable_ipv6_candidates`, and `helper_role`.
- Added an encrypted DHT rendezvous adapter interface with in-memory and file-backed local stores. This gives deterministic fake-DHT tests and a private-store diagnostic path without exposing plaintext SDP/candidates.
- Wired the runtime snapshot flow so DHT mode can publish/fetch encrypted offer/answer/candidate snapshots through that adapter.
- Added first-pass DHT/NAT diagnostics for helper path status, IPv6 candidates, DHT-exchanged remote candidates, port-mapping placeholder state, and relay-required style failure classes.
- Still pending: libtorrent Mainline DHT network backend, miniupnpc UPnP IGD mapping, PCP/NAT-PMP, and real cross-LAN validation evidence.

1. Add runtime config surface:
   - `signal_transport=dht`
   - `dht_bootstrap=<host:port list>`
   - `enable_port_mapping=true|false`
   - `enable_ipv6_candidates=true|false`
   - `helper_role=off|client|lan-helper|relay`
2. Implement encrypted DHT publish/lookup for offer/answer and trickle candidate records.
3. Add NAT mapper module for UPnP IGD, PCP, and NAT-PMP, with explicit diagnostics.
4. Add synchronized UDP hole-punch orchestration around ICE candidate exchange.
5. Add optional LAN helper service mode.
6. Add local diagnostic storage using encrypted blobs in `signal_dir/dht-mailbox`.

## Acceptance Criteria

Serverless DHT mode passes when:

- UI pairing uses machine code only; no IP address is required.
- No RedClaw-owned public rendezvous server is configured.
- No paid third-party relay is required for the attempted path.
- DHT rendezvous publishes and retrieves encrypted offer/answer records.
- ICE candidate diagnostics include LAN, IPv6, mapped, STUN, and DHT-exchanged candidates where available.
- If a direct path succeeds, Controller UI displays decoded Host desktop video.
- If the path fails, logs classify the blocker as missing DHT, no IPv6, no port mapping, symmetric/CGNAT risk, firewall, or relay required.
