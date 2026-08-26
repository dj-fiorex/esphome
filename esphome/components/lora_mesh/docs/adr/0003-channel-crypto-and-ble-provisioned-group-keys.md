---
status: superseded by ADR-0008
---

# Per-group payload cryptography with BLE-provisioned keys

The product is a unified mesh "divided in software" between tenants (condo, museum, etc.), and nodes drive water valves/pumps. On a shared RF medium the only thing preventing one tenant from reading or commanding another tenant's actuators is cryptography; the original cleartext `mesh_secret`→`mesh_id` provided isolation only, not confidentiality or authentication, so any sniffer could spoof a valve command.

**Decision:** split identity into two layers and add authenticated encryption:
- **Fabric ID** — a shared, non-secret identifier (YAML constant) that gates Forwarding/membership so all VasoSmart Nodes Forward for each other (the "unified mesh"). No security role.
- **Group key** — a per-tenant secret, **provisioned at runtime over BLE** by the existing mobile app and **persisted in NVS**. Used to authenticate-encrypt the DATA **payload** with **AES-128-CCM** (hardware-accelerated on ESP32-S3), plus a MIC and a **persistent frame counter** for replay protection.

Routing-visible header fields (Fabric ID, src/dst/next-hop hashes, msg_id, TTL) and HELLO/routing stay **cleartext** so Forwarding Nodes without the group key can still Forward. Only payloads are protected. Nodes start **unprovisioned** (no key → inert) until paired.

**Why:** confidentiality + command authentication are mandatory once tenants share one physical mesh and control water. BLE provisioning already exists in the product, so keys live in NVS, not YAML. See [[lora-mesh-security-model]].

**Consequences:**
- The frame counter (was `msg_id`, which reset to 0 on reboot) **must** be NVS-persisted: nonce reuse under one key breaks the cipher.
- **Metadata leaks** — topology and who-talks-to-whom are observable; only payloads are hidden. Accepted.
- `mesh_secret` (compile-time YAML) is replaced by runtime key management; this is a config-surface breaking change.
