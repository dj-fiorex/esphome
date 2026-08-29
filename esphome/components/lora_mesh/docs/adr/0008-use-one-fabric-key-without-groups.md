# Use one Fabric Key without Groups

The first product delivery has one Fabric and no tenant or Group partitions. All Jocondo Nodes share one Fabric Key for AES-CCM payload authentication and encryption, and every reachable Gateway can serve every Node; this removes Group identifiers, multi-key storage, and Group-specific Gateway discovery while preserving protection against forged actuator commands.

Multi-Group isolation is deliberately deferred until product demand justifies its provisioning, routing, and gateway complexity.

For the first release, the Fabric Key is a mandatory configuration value provided at build time through ESPHome's secret configuration and must not be committed to the repository. Configuration fails without a valid 128-bit key, and every DATA packet is authenticated-encrypted. Runtime key setters, key clearing, NVS key persistence, unprovisioned operation, and plaintext DATA are removed; BLE provisioning and key rotation are deferred. The configuration and public API use the Fabric terminology rather than retaining Group aliases.

Protocol version 4 uses AES-128-CCM with an eight-byte MIC for DATA. The extra four authentication bytes are accepted because no deployed protocol requires compatibility and actuator-command forgery resistance is more important than retaining the former 222-byte maximum; the resulting maximum DATA payload is 218 bytes.

The Fabric Key is the only configured mesh identity value. The component derives a public 32-bit Fabric ID from it with domain-separated cryptographic derivation and carries that ID in cleartext for fast rejection of unrelated traffic. The old `mesh_secret` option and a separately configured Fabric ID are removed, preventing identity/key mismatches while keeping the secret key itself off the wire.
