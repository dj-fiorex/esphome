# lora_mesh wire format (protocol version 4)

Protocol version 4 uses one mandatory 128-bit Fabric Key. The key is never sent on air. It derives the public
Fabric ID and authenticates-encrypts every DATA payload. There is no Group identifier, unprovisioned state,
plaintext DATA mode, or version-3 compatibility path.

All multi-byte integers are little-endian. Node identities are 32-bit FNV-1a hashes of Node ID strings.

## Fabric identity derivation

The public Fabric ID is derived from the Fabric Key with a domain-separated AES-128-CCM pseudorandom function:

1. Use the 13-byte ASCII nonce `LORA-FABRICID`.
2. Authenticate empty plaintext with the ASCII AAD `LORA-MESH-ID-v1` under the Fabric Key and request an
   eight-byte CCM tag.
3. Interpret the first four tag bytes as a little-endian unsigned integer.

This deterministic identifier lets a Node discard unrelated Fabric traffic before deeper processing. It is public
and has no independent security role.

## Header — 28 bytes, every packet

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 4 | `fabric_id` | Public ID derived from the Fabric Key. Mismatch means drop. |
| 4 | 1 | `pkt_type` | `1`=HELLO, `2`=DATA; remaining values are reserved. |
| 5 | 1 | `flags` | `0x01` gateway, `0x02` broadcast, `0x04` ACK requested, `0x08` forwarded. |
| 6 | 4 | `src_id` | Origin Node; immutable end to end and included in DATA AAD. |
| 10 | 4 | `dst_id` | Final destination or `0xFFFFFFFF` broadcast; included in DATA AAD. |
| 14 | 4 | `frame_counter` | Persistent sender counter; included in the DATA nonce and AAD. |
| 18 | 1 | `ttl` | Remaining hops; decremented by a Forwarding Node. |
| 19 | 1 | `hop_count` | Traversed hops; incremented by a Forwarding Node. |
| 20 | 4 | `prev_hop` | Link sender; rewritten on each Forward. |
| 24 | 4 | `next_hop` | Intended Forwarding Node; `0xFFFFFFFF` for broadcast. |

The mutable routing fields are not in DATA AAD. `src_id`, `dst_id`, `frame_counter`, packet type, and payload
length are authenticated end to end.

## DATA body

```
[28]        payload_len  (1)   application plaintext length P (0..218)
[29]        ciphertext   (P)   AES-128-CCM ciphertext
[29+P]      tag          (8)   AES-128-CCM authentication tag
```

The maximum application payload is `255 - 28 - 1 - 8 = 218` bytes. Longer application strings are truncated to
that supported limit before encryption.

### AES-128-CCM construction

- Key: the 16-byte Fabric Key configured at build time.
- Nonce (13 bytes): `src_id[4] || dst_id[4] || frame_counter[4] || 0x00`.
- AAD (14 bytes): `pkt_type || src_id || dst_id || frame_counter || payload_len`.
- Authentication tag: eight bytes.

Every DATA send follows this construction. A receiver delivers a payload only after the tag verifies and the
sender counter passes replay checking. Wrong-key, modified, truncated, and replayed DATA does not reach the
application callback.

## HELLO body

```
[28]          proto_version (1)   = 4
[29]          name_len      (1)   N (0..32)
[30]          node_name     (N)   not NUL-terminated
[30+N]        route_count   (1)   R
[31+N]        routes        (R*6) RouteAdvertisement[]
```

A Route advertisement is `dest_id[4] || hop_count[1] || rssi_scaled[1]`. HELLO is single-hop and never
Forwarded. A HELLO whose protocol byte is not 4 is rejected before it can create a direct or advertised Route.
HELLO authentication is outside the DATA-security contract implemented here.

## Persistent counters and replay protection

The sender counter is part of the CCM nonce, so a Node must not reuse it with the same Fabric Key and source ID.
The component writes a counter reservation ahead to ESPHome preferences in batches of 1000. After reboot it
resumes from the reserved value, safely skipping unused values instead of risking reuse.

Receivers keep a runtime high-water counter per source. After successful tag verification they reject counters at
or below the high-water value, then advance it for accepted DATA. The receiver high-water table is intentionally
runtime-only; durable application command sequencing owns reboot-safe actuator idempotency.

## Receive and Forwarding summary

1. Reject a mismatched Fabric ID or the Node's own echoed source ID.
2. Reject an already-seen `(src_id, frame_counter)` pair.
3. For HELLO, require protocol version 4 before changing Route or name state.
4. For DATA addressed to this Node or broadcast, verify/decrypt with the Fabric Key, replay-check, then deliver.
5. Forward eligible DATA by copying the authenticated-encrypted body unchanged and rewriting only `ttl`,
   `hop_count`, `prev_hop`, and `next_hop`.

Unicast remains single-path through the designated Next Hop. Broadcast remains flood-based. Both carry only
authenticated-encrypted application payloads.
