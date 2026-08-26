# lora_mesh wire format (protocol version 4)

Protocol version 4 uses one mandatory 128-bit Fabric Key. The key is never sent on air. It derives the public
Fabric ID, authenticates-encrypts every DATA payload, and derives a separate key for authenticated HELLO traffic.
There is no Group identifier, unprovisioned state, plaintext DATA mode, or version-3 compatibility path.

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
| 5 | 1 | `flags` | `0x01` sender has Upstream Connectivity, `0x02` broadcast, `0x04` ACK requested, `0x08` forwarded. |
| 6 | 4 | `src_id` | Origin Node; immutable end to end and included in DATA AAD. |
| 10 | 4 | `dst_id` | Final destination or `0xFFFFFFFF` broadcast; included in DATA AAD. |
| 14 | 4 | `frame_counter` | Persistent sender counter; included in the DATA nonce and AAD. |
| 18 | 1 | `ttl` | Remaining hops; decremented by a Forwarding Node. |
| 19 | 1 | `hop_count` | Traversed hops; incremented by a Forwarding Node. |
| 20 | 4 | `prev_hop` | Link sender; rewritten on each Forward. |
| 24 | 4 | `next_hop` | Intended Forwarding Node; `0xFFFFFFFF` for broadcast. |

The mutable routing fields are not in DATA AAD. Flags, `src_id`, `dst_id`, `frame_counter`, packet type, and payload
length are authenticated end to end for DATA. Every header byte is authenticated for HELLO because HELLO is never
Forwarded.

## DATA body

```
[28]        payload_len  (1)   application plaintext length P (0..218)
[29]        ciphertext   (P)   AES-128-CCM ciphertext
[29+P]      tag          (8)   AES-128-CCM authentication tag
```

The maximum application payload is `255 - 28 - 1 - 8 = 218` bytes. Longer application strings are truncated to
that supported limit before encryption. The body must have exactly `1 + P + 8` bytes; receiving and Forwarding Nodes reject
truncated bodies, legacy four-byte tags, and trailing bytes before delivery or forwarding.

### AES-128-CCM construction

- Key: the 16-byte Fabric Key configured at build time.
- Nonce (13 bytes): `src_id[4] || dst_id[4] || frame_counter[4] || 0x00`.
- AAD (15 bytes): `pkt_type || flags || src_id || dst_id || frame_counter || payload_len`.
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
[31+N]        routes        (R*7) RouteAdvertisement[]
[31+N+R*7]    tag           (8)   truncated HMAC-SHA256
```

A Route advertisement is `dest_id[4] || hop_count[1] || path_rssi[1] || route_flags[1]`. Route flag bit `0x01`
means the destination advertises Upstream Connectivity and is therefore a Gateway. Path RSSI is a signed dBm value;
receivers extend a Route with the minimum of this advertised value and the newly measured link RSSI.

The control-plane key is `HMAC-SHA256(Fabric Key, "LORA-MESH-CONTROL-v1")`. The HELLO tag is the first eight bytes
of `HMAC-SHA256(control-plane key, complete 28-byte header || complete HELLO body)`. Routing metadata stays visible.

HELLO is single-hop and never Forwarded. Its destination and Next Hop are broadcast, its Previous Hop equals its
source, its hop count is zero, and only the Upstream Connectivity flag is valid. Advertisements use a non-broadcast
destination, a hop count in `[1, 254]`, and only the Gateway Route flag. A receiver requires these invariants,
protocol version 4, a Node name no longer than 32 bytes, the exact packet length implied by Route count, and a valid
tag before it updates duplicate suppression, Node names, direct or advertised Routes, Gateway visibility,
diagnostics, or callbacks.

When Gateway eligibility changes, the Gateway schedules a coalesced, rate-limited HELLO. A Node that learns the
promotion or Gateway Withdrawal does the same, so the final state advances one hop after each immediate-update window
without waiting one periodic HELLO interval per hop. The rate limiter preserves a pending update while states flap;
the HELLO is built at enqueue time and therefore advertises the final stable Gateway state. Changed Gateway Routes
take priority when the Route table exceeds one HELLO's capacity, and unadvertised changes remain pending for later
rate-limited HELLOs. A Route with a pending Gateway change is protected from table-pressure eviction until one of
those HELLOs advertises its final state.

Abrupt disappearance has no withdrawal packet. Ordinary neighbour and dependent-Route expiry remains the only failure
detector. The production defaults pair a 30-second periodic HELLO with a 90-second Route lease, approximately three
missed HELLOs; the next `send_to_gateway()` call selects from the Routes still valid after expiry.

## Persistent counters and replay protection

The sender counter is part of the CCM nonce, so a Node must not reuse it with the same Fabric Key and source ID.
The component writes a counter reservation ahead to ESPHome preferences in batches of 1000. After reboot it
resumes from the reserved value, safely skipping unused values instead of risking reuse.

Receivers keep a runtime high-water counter per source. After successful tag verification they reject counters at
or below the high-water value, then advance it for accepted DATA. The receiver high-water table is intentionally
runtime-only; durable application command sequencing owns reboot-safe actuator idempotency.

## Receive and Forwarding summary

1. Reject a mismatched Fabric ID or the Node's own echoed source ID.
2. For HELLO, validate its version, exact shape, and HMAC before consulting or changing the Seen-cache.
3. Reject an already-seen `(src_id, frame_counter)` pair.
4. For authenticated HELLO, update direct and advertised Routes and Node names.
5. For DATA addressed to this Node or broadcast, verify/decrypt with the Fabric Key, replay-check, then deliver.
6. Forward eligible DATA by copying the authenticated-encrypted body unchanged and rewriting only `ttl`,
   `hop_count`, `prev_hop`, and `next_hop`.

Unicast remains single-path through the designated Next Hop. Broadcast remains flood-based. Both carry only
authenticated-encrypted application payloads.
