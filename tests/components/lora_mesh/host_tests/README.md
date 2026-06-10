# lora_mesh host tests

Behavior tests for the mesh protocol logic (wire format, routing, single-path
unicast forwarding, flood broadcast, duplicate suppression) that run natively
on the development machine — no hardware, no PlatformIO.

```bash
tests/components/lora_mesh/host_tests/run.sh
```

How it works:

- `lora_mesh.cpp` is compiled against the real `esphome/` headers; only
  `esphome/core/defines.h` is shadowed by `stub_include/` so no optional
  feature (sensors, wifi, radio drivers) is pulled in.
- `stubs.cpp` provides the handful of runtime symbols the component links
  against (`millis()` as a controllable fake clock, `random_uint32()`,
  `Component` method definitions).
- Tests drive a `LoraMesh` instance exclusively through its public interface:
  config setters, `setup()`/`loop()`, the send APIs, `on_radio_packet()`, and
  callbacks. Incoming packets are built by the test from the spec in
  `esphome/components/lora_mesh/docs/wire-format.md`, independent of the
  component's own builders, so the tests verify the on-air format itself.

These complement (not replace) the YAML compile tests in the parent directory,
which verify config validation and that the component builds for real targets.
