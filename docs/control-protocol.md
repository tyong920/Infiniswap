# Infiniswap Control Protocol 1.x

The control protocol is the bounded request/response channel used before a
Memory Provider registers or grants Remote Memory. The normative constants and
codec API are in `common/infiniswap_protocol.h`.

## Framing

Every integer is unsigned and encoded in network byte order. A frame is exactly
48 bytes of header followed by `payload_length` bytes, with a hard maximum of
4096 bytes. Receivers reject both truncation and trailing bytes.

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `ISWP` (`0x49535750`) |
| 4 | 2 | Major version |
| 6 | 2 | Minor version |
| 8 | 2 | Message type |
| 10 | 2 | Flags (`RESPONSE`, `ERROR`; all other bits zero) |
| 12 | 4 | Payload length |
| 16 | 8 | Monotonic request identifier |
| 24 | 8 | Session identifier, zero before acceptance |
| 32 | 8 | Offered or negotiated capabilities |
| 40 | 8 | Required capabilities |

Major versions must match. Adjacent minor builds accept each other's `HELLO`
and negotiate the lower minor version, so both the 1.0 and 1.1 binaries can
start a 1.0 session. Unknown required capabilities fail the handshake; unknown
offered capabilities are ignored unless shared. Consumer-originated requests
use a monotonic sequence. Responses set `RESPONSE` and echo the request
identifier; Provider-originated requests use an independent monotonic sequence.
Identifiers may not be zero.

## Capabilities

Bits currently represent Backed Mode, Remote-Only Mode, Opportunistic Pool,
Committed Pool, Provider Failure Deadline, status reporting, and HMAC-SHA256
authentication. Each peer advertises only the capabilities its current runtime
can honor; negotiation selects the intersection, so a Provider without
Committed Pool accounting rejects a Remote-Only/Committed selection. The
selected operating mode and pool must both be present in the negotiated set.
Provider Failure Deadline, status reporting, and HMAC-SHA256 are required by the
Provider so every accepted session has authenticated liveness detection.

## Messages

| Type | Direction | Payload |
| --- | --- | --- |
| `HELLO` | Consumer to Provider | Consumer/key IDs, operating mode, pool, Provider Failure Deadline, Consumer nonce |
| `CHALLENGE` | Provider to Consumer | Provider nonce and negotiated minor version |
| `AUTH` | Consumer to Provider | 32-byte HMAC-SHA256 tag |
| `ACCEPT` | Provider to Consumer | Negotiated minor version; header carries the new session ID |
| `STATUS_REQUEST` | Consumer to Provider | Empty |
| `STATUS_RESPONSE` | Provider to Consumer | Available Opportunistic/Committed chunks, deadline, status flags |
| `CHUNK_REQUEST` | Consumer to Provider | Count, logical start, pool |
| `CHUNK_GRANT` | Provider to Consumer | Bounded list of logical/provider chunk IDs, address, rkey, and pool |
| `EVICT` | Provider to Consumer | Bounded list of Provider chunk IDs |
| `ACTIVITY` | Consumer response | Bounded Provider chunk ID/activity pairs |
| `RELEASE` | Provider request / Consumer response | Bounded list of Provider chunk IDs |
| `GOODBYE` | Either direction | Empty |
| `ERROR` | Response in either direction | Error code, offending type, retryable bit |

Reserved bytes are encoded as zero. Counts are checked before multiplication and
are limited to 128 entries per frame. Compile-time Memory Consumer or Provider
capacity settings do not change any header, entry, or maximum frame size.

## Authentication

The Provider sends no capacity information and registers no Remote Memory until
it has accepted the handshake. The authentication tag is:

```text
HMAC-SHA256(
  PSK,
  "Infiniswap control authentication v1" ||
  exact HELLO frame ||
  exact CHALLENGE frame
)
```

The Provider selects the PSK by Consumer ID and key ID. Each allowlist entry may
hold a current and next key for a rotation overlap bounded by the next key's
`next_valid_until_unix` timestamp. Authentication must complete within five
seconds. A next-key session is disconnected at expiry; removing, revoking, or
changing an active Consumer's authorization disconnects it immediately when
the mode-0600 allowlist is reloaded. The PSK is never sent on the wire and is
excluded from protocol errors and logs.

## State and failure rules

The only valid handshake order is `HELLO`, `CHALLENGE`, `AUTH`, `ACCEPT`.
Authenticated requests must use the accepted session ID, negotiated version and
capabilities, and a request ID greater than the previous request. Malformed,
replayed, unauthenticated, revoked, unsupported, and out-of-order messages
receive a structured `ERROR`, after which the Provider closes the connection.
Connection teardown deregisters every Remote Memory region owned by that
session, so a protocol failure cannot leave a grant active.

Backed Mode selects the Opportunistic Pool and requests individual Hot Ranges.
Remote-Only Mode selects the Committed Pool and sends one full-capacity
`CHUNK_REQUEST` before exposing its block device. The Provider admits that
request transactionally; a short grant is invalid. Committed grants are never
included in `EVICT` or Provider-originated `RELEASE` requests. Every active
session sends authenticated status heartbeats at one third of the Provider
Failure Deadline. Each valid authenticated Consumer frame refreshes the
Provider's liveness deadline; silence through the full deadline disconnects the
session and returns its Remote Chunks to the applicable pool. A failed Backed
Mode heartbeat makes the Consumer fall back to its Backing Store, while any
connection, heartbeat, or operation failure after a Remote-Only reservation
makes the Consumer enter terminal Remote-Lost and reject subsequent I/O.
