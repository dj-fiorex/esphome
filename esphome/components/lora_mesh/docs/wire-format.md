# lora_mesh wire format (proto version 3)

This is the on-air format implied by ADRs 0002 (single-path unicast) and 0003 (per-group
crypto). It is a **breaking change** vs. the v2 format currently in the code: the header
grows, `msg_id` becomes a persistent `frame_counter`, a `next_hop` field is added, and DATA
payloads are authenticated-encrypted. `MESH_PROTO_VERSION` bumps to **3**.

All multi-byte integers are **little-endian**. All node identities are 32-bit FNV-1a hashes.

---

## 1. Layering (what is protected, what is not)

| Layer | Fields | Visibility | Integrity |
|-------|--------|-----------|-----------|
| **Fabric / link** | fabric_id, pkt_type, flags, ttl, hop_count, prev_hop, next_hop | cleartext | none (mutable per hop) |
| **End-to-end identity** | src_id, dst_id, frame_counter | cleartext | bound into the payload MIC (AAD) |
| **Payload (DATA only)** | application bytes | **encrypted** (per-Group key) | **MIC** (per-Group key) |
| **HELLO body** | name, route digest | cleartext | none |

Relays forward packets **without the Group Key** — they only read the cleartext
fabric/link layer. Only Group members can decrypt/verify the payload. This is what makes
"one unified Fabric, divided into Groups in software" work.

---

## 2. Header — 28 bytes, every packet

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 4 | `fabric_id` | FNV-1a of the (non-secret) fabric name. Mismatch ⇒ drop. Relay gate only. |
| 4 | 1 | `pkt_type` | `1`=HELLO, `2`=DATA, `3`=ROUTE_REQUEST*, `4`=ROUTE_REPLY*, `5`=ACK*, `6`=ERROR* |
| 5 | 1 | `flags` | bitmask, see §3. **Not authenticated** (routing-layer only). |
| 6 | 4 | `src_id` | Origin node. Immutable end-to-end. In MIC AAD. |
| 10 | 4 | `dst_id` | Final destination, `0xFFFFFFFF`=broadcast. Immutable. In MIC AAD. |
| 14 | 4 | `frame_counter` | Per-source monotonic, **NVS-persisted** (§6). Dedup + crypto nonce. In MIC AAD. |
| 18 | 1 | `ttl` | Remaining hops. Decremented per forward. Mutable. |
| 19 | 1 | `hop_count` | Hops taken. Incremented per forward. Mutable. |
| 20 | 4 | `prev_hop` | Link-layer sender of *this* hop. Rewritten on each forward. |
| 24 | 4 | `next_hop` | Intended forwarder; `0xFFFFFFFF`=any (broadcast/flood). Rewritten per forward. |

\* reserved, not yet implemented.

`prev_hop`/`next_hop` are the **per-hop** link addresses; `src_id`/`dst_id` are the
**end-to-end** addresses and never change while forwarding.

---

## 3. Flags (byte 5)

| Bit | Name | Meaning |
|-----|------|---------|
| 0x01 | `IS_GATEWAY` | Originator currently advertises gateway capability |
| 0x02 | `IS_BROADCAST` | `dst_id` is the broadcast address |
| 0x04 | `ACK_REQUESTED` | Reserved — application-level ACK is preferred (ADR 0002) |
| 0x08 | `IS_FORWARD` | Set when a node retransmits someone else's packet |

Flags are deliberately **outside** the MIC: they are routing-layer hints that legitimately
change at relays. Tampering with them can disrupt routing (a residual DoS risk, §7) but
cannot forge or replay a payload.

---

## 4. DATA body (offset 28)

```
[28]        payload_len   (1)   plaintext length P (0..222)
[29]        ciphertext    (P)   AES-128-CCM encryption of the P plaintext bytes
[29+P]      mic           (4)   CCM authentication tag (truncated to 4 bytes)
```

Max `P` = `255 - 28 - 1 - 4` = **222 bytes**.

### Crypto construction (AES-128-CCM, per-Group Key)

- **Key**: the 16-byte Group Key (BLE-provisioned, NVS-persisted). A node that holds no
  key is unprovisioned and processes no DATA.
- **Nonce (13 bytes)**: `src_id[4] || dst_id[4] || frame_counter[4] || 0x00`.
  Unique per key as long as `frame_counter` never repeats for a given `src_id` (⇒ §6).
- **AAD (14 bytes)**: `pkt_type || src_id || dst_id || frame_counter || payload_len`.
  Binds the ciphertext to its origin, destination, counter and length so a captured
  payload cannot be replayed to a different destination or attributed to a different source.
- **MIC**: CCM tag truncated to **4 bytes** (LoRaWAN-style; per-packet forgery probability
  2⁻³². See §8 for the 8-byte option).

### Broadcast is *group*-broadcast

A broadcast DATA packet is still encrypted under a Group Key, so "broadcast" reaches all
members **of that Group** (relayed by everyone via flood, readable only by the Group). There
is no fabric-wide cleartext broadcast for application data.

---

## 5. HELLO body (offset 28) — cleartext, not encrypted

```
[28]          proto_version (1)   = 3
[29]          name_len      (1)   N (0..32)
[30]          node_name     (N)   not NUL-terminated on wire
[30+N]        route_count   (1)   R
[31+N]        route entries (R*6) RouteAdvertisement[]
```

`RouteAdvertisement` (6 bytes): `dest_id[4] || hop_count[1] || rssi_scaled[1 (int8)]`.

HELLO is **single-hop** (never forwarded; `next_hop`=broadcast, but relays do not
re-broadcast HELLO). It carries no MIC — routing is fabric-level and the fabric has no
shared secret to authenticate with (§7).

---

## 6. Frame counter & replay protection

- `frame_counter` replaces v2 `msg_id`. It is the **CCM nonce input**, so reuse under one
  key **breaks the cipher** — it must never repeat for a given source.
- **Persist across reboots in NVS, batched**: reserve a block (e.g. write
  `counter + 1000`), hand out counters until the block is exhausted, then persist the next
  block. On boot, resume from the persisted (already-ahead) value. This guarantees no reuse
  while writing flash only once per ~1000 packets (avoids flash wear). Skipped counters are
  harmless.
- **Receiver replay check**: maintain a per-`src_id` high-water `frame_counter`. Verify the
  MIC **first**, then reject if `frame_counter <= high_water[src_id]`, then update it. (MIC
  before counter so a forged high counter cannot DoS the high-water mark.)
- On 32-bit wrap (not reachable in a watering node's lifetime at normal rates) the Group Key
  must be rotated.

---

## 7. Receive pipeline (summary)

1. `fabric_id` mismatch ⇒ drop.
2. `src_id == self` ⇒ drop (own echo).
3. Seen-cache `(src_id, frame_counter)` hit ⇒ drop (loop/dup suppression, all types).
4. Dispatch by `pkt_type`:
   - **HELLO** ⇒ update direct route + parse cleartext digest (no crypto).
   - **DATA**:
     - If `dst==self` or broadcast: for each held Group Key, CCM-decrypt+verify. On MIC
       success ⇒ replay-check (§6) ⇒ deliver plaintext to `on_message`. MIC fail on all
       keys ⇒ not our Group, drop.
     - **Forward** (relay) — *no key needed*: only if `forward_messages` && `ttl>1` &&
       (broadcast **or** `next_hop==self`). Patch `ttl-1`, `hop_count+1`,
       `prev_hop=self`, and for unicast set `next_hop` = next hop toward `dst` from the
       routing table; for broadcast set `next_hop`=broadcast. Enqueue (§ TX queue, ADR
       transmit-model) rather than transmit inline.

A gateway serving multiple Groups holds multiple keys and tries each in step 4; a normal
pot holds one.

---

## 8. Decided parameters

- **MIC size = 4 bytes.** LoRaWAN-style, airtime-cheap, 2⁻³² forgery probability per packet.
  Acceptable because valve/pump commands also sit behind an application-level ACK (ADR 0002).
- **HELLO / routing is NOT authenticated.** The fabric is non-secret, so there is no key to
  sign with without shipping a global, extractable secret in firmware. Accepted residual risk
  (see below).
- **Header = 28 bytes**, including `prev_hop` (kept for `MeshMessage.prev_hop` and future
  link/neighbor-quality diagnostics).

## 9. Security properties & known limitations

**Guaranteed (per Group Key):** payload confidentiality, command authentication, and replay
protection (NVS-persisted frame counter + per-source high-water). A node without the Group
Key cannot read or forge a Group's payloads.

**Known limitation — routing-layer DoS.** Because the fabric/link layer and HELLO are
cleartext and unauthenticated, an attacker within RF range can inject false routes
(sinkhole/blackhole) or tamper with `flags`/`ttl` to disrupt delivery. This is the same
severity class as RF jamming and does **not** compromise payload confidentiality, command
authentication, or replay protection. Mitigation if it ever becomes a real threat: a
fabric-level signing key for HELLOs (deferred).
