# lora_mesh host tests

Behavior tests for protocol-v4 authenticated discovery, Upstream Connectivity,
Nearest Gateway routing, single-path unicast Forwarding, flood broadcast,
Gateway promotion and Withdrawal, distributed HELLO/lease expiry and Route
Hold-down, duplicate suppression, DATA security, and replay handling. They run natively
on the development machine — no hardware and no PlatformIO.

```bash
tests/components/lora_mesh/host_tests/run.sh
```

How it works:

- `lora_mesh.cpp` is compiled against the real `esphome/` headers; only
  `esphome/core/defines.h` is shadowed by `stub_include/` so no optional
  feature (sensors or radio drivers) is pulled in.
- `stubs.cpp` provides the handful of runtime symbols the component links
  against (`millis()` as a controllable fake clock, `random_uint32()`,
  `Component` method definitions).
- Tests drive a `LoraMesh` instance exclusively through its public interface:
  config setters, `setup()`/`loop()`, the send APIs, `on_radio_packet()`, and
  callbacks. Incoming packets are built by the test from the spec in
  `esphome/components/lora_mesh/docs/wire-format.md`, independent of the
  component's own builders. A fixed independently calculated HMAC vector checks
  the HELLO control-plane derivation and tag format.

These complement (not replace) the YAML compile tests in the parent directory,
which verify config validation and that the component builds for real targets.
