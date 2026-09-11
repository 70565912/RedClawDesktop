# Compressed Protobuf application wire v1

Task: P1-AG-01R serialization follow-up, 2026-09-09.
Status: implemented; local tests and Debug/Release gates are recorded in PROJECT_STATE.
Physical paired acceptance remains a separate gate.

## Contract and purpose

The previous Control/Agent key-value messages repeated field names and defaults.
The new producer serializes typed fields from `src/protocol/schema/redclaw_wire.proto`
and compresses the **whole serialized message** with Zstd level 1 and checksum.
It does not place the old text envelope inside a Protobuf string.

Each application frame is `RCP1 | kind:u8 | one complete Zstd frame`:

| Kind | Protobuf payload | Delivery |
| --- | --- | --- |
| 1 | StreamControlMessageV1 | binary redclaw-control-v1 |
| 2 | AgentMessageEnvelopeV1 | binary redclaw-agent-v1 |
| 3 | DhtSignalSnapshot, schema 5 | compressed before existing RCE1 encryption/storage |
| 4 | DebugBridgeEnvelopeV1 | binary Debug Bridge channel |

Compression is independent per message, with no inter-message dictionary or
uncompressed fallback. Loss, reconnect and Agent-only recreation need no preceding
compression state. Compressor/decompressor contexts are reused per thread to avoid
repeated setup; their reuse does not introduce a wire dependency.

Protobuf field numbers and enum values are permanent contracts: do not renumber
or reuse them. Existing domain schema versions, epoch, IDs, validation and replay
rules remain in force. Generated `.pb.cc/.pb.h` are build artifacts, not committed.

## Bounds and ordering

- Compressed frame and expanded Protobuf are independently limited to 64 KiB.
- Unknown expanded lengths, truncated/corrupt frames, trailing bytes, concatenated
  frames, wrong kinds and invalid enum/narrowing values are rejected before use.
- Protobuf parser recursion is bounded to 16; repeated-field domain caps remain.
- Instructions remain 16 KiB, text chunks 8 KiB, input batches 64 events and 4 KiB
  **expanded Protobuf**, not 4 KiB after compression.
- Incoming execution queues account expanded bytes. Tiny compressed inputs cannot
  multiply accepted queued memory beyond the existing bounds.
- Stream chunks and opaque payloads are `bytes`, preserving binary and split UTF-8
  bytes exactly; identifiers and names remain typed strings with domain validation.
- Complete create/start/steer instructions require valid UTF-8 scalar sequences;
  byte-preserving stream chunks do not weaken that instruction boundary.
- Media payloads, ICE/STUN/DTLS/SCTP and DHT storage framing are not re-encoded or
  re-compressed. Existing media two-slot/latest-only and Agent scheduling stay intact.

DHT's 700-byte direct-store decision uses the actual compressed, encrypted size.
Indirect tests use deterministic incompressible content; repeating one character
no longer proves chunk publication. Chunk ordering, immutable ciphertext retries,
expiry, request/instance tags and publication scheduling are unchanged. Storage
summary metadata and outer bencode remain their existing transport format.
The Answer-ACK mutation regression also finds its boundary through the production
compressed-size estimator: it still proves that echoing an ACK could cross 700
bytes, and that the production coordinator does not mutate the pending Answer.

## Local API, upgrade and persistence

GUI/runtime stdout/stdin retains its named Base64 line boundary, now wrapping RCP1.
The entire local line remains at most 64 KiB; Base64 is strictly canonical. Network
DataChannels send binary directly, without this local-only Base64 overhead.

`invoke-agent-control.ps1` uses the matching published `redclaw_protocol_codec.exe`
for encode/decode; it no longer implements a second key-value wire codec. JSON is
only local helper stdin/stdout, not the network format. The helper neither executes
tasks nor accesses providers. Instructions do not appear in command-line arguments
or diagnostics. Windows PowerShell 5.1 writes explicit UTF-8 bytes, avoiding its
missing StandardInputEncoding API and local console-code-page conversion.
The restricted local Debug QA pipe and human-readable status/evidence files remain
JSON. They are not peer-network message serialization and do not add wire overhead.

The official local API's `status` operation returns the last locally confirmed
snapshot (`event_kind=local_snapshot`) without sending a network sync or appending
a journal record. It is not a claim of a newly fetched peer state. Only explicit
`sync` requests replay missing events; status polling must not repeatedly request
the entire output from ACK zero. Local ACK/query records never overwrite the
peer's observed approval or terminal state with a dispatched/running state.
Provider terminal events cancel any outstanding approval lease. Finished/paused
turns reject late provider callbacks until an explicit follow-up takes ownership;
otherwise an already interrupted task could become paused at the old approval
deadline. This lifecycle rule is independent of compression and message replay.
Every new GUI command also has its own request ID before entering coordination;
approval decisions use the peer's pending approval ID. GUI and formal API status
are tested separately, so a locally rejected GUI interrupt cannot be masked by
success through another entry point.

Host and Controller, GUI/runtime, Debug Bridge and API helper must upgrade together.
An old Control/Agent text frame is rejected, not silently retried through an old
network format. Old DHT V3/V4 records can still be read until they expire; new
publishers emit only schema 5. Reading an old record does not authorize an old
desktop endpoint as wire-compatible.

Only the Broker's local completed-task metadata reader imports old metadata lines.
It cannot import instructions or execute a task, and it is not used by network/API
parsers. Reads are non-mutating; the next normal persistence writes the new format.
Unknown/corrupt history is preserved instead of overwritten by an empty history.

## Evidence and acceptance

Production-code representative fixture, Debug, before/after:

| Fixture | Previous wire bytes | Compressed Protobuf bytes |
| --- | ---: | ---: |
| Ping | 1547 | 46 |
| 5920-byte patterned Agent log block | 6696 | 403 |

These are compressible fixtures, not a promised universal
compression ratio or physical connection-speed improvement. Random bytes can grow
slightly due to framing. Tests record that case without asserting impossible savings.
The 8192-byte deterministic random payload is 8256 bytes on the wire, including its
typed envelope; the regression records exact byte preservation.

Regression coverage uses production codecs: independent frames, binary chunks,
malformed/oversized data, input narrowing, local CLI Unicode and boundaries,
bidirectional routing, metadata import, and DHT direct/indirect publication.
Full non-E2E, standard builds and fixed-path public-DHT dual GUI remain required.
Reduced message bytes alone do not close the existing GUI/input pressure failure.

See [Agent architecture](remote-development-agent-bridge-v1.md) and
paired role-swap gates in the current integration suite.
