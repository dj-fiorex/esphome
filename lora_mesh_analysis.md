# `lora_mesh` Component — ESPHome Best-Practice Analysis

> **Branch:** `feat/cmp-lora-mesh-implementation` · **Repo:** `dj-fiorex/esphome`  
> **Reference guide:** `.ai/instructions.md` (ESPHome AI Collaboration Guide)  
> **Analysis date:** 2026-05-09

---

## 1. Component Structure

| Check | Status | Notes |
|---|---|---|
| `__init__.py` present | ✅ | Complete and well-organised |
| Header file (`lora_mesh.h`) | ✅ | Clean, single-responsibility header |
| Implementation file (`lora_mesh.cpp`) | ✅ | Matches header |
| Extra headers logically split | ✅ | `lora_packet.h`, `lora_radio.h`, `lora_radio_adapters.h`, `automation.h` |
| Platform-specific sub-directory | ➖ N/A | Protocol-level component; no platform-specific C++ needed |
| `README.md` | ✅ | Bonus; comprehensive user documentation |

**Verdict:** Structure is clean and follows ESPHome conventions.

---

## 2. Component Metadata

| Metadata key | Status | Notes |
|---|---|---|
| `CODEOWNERS` | ✅ | `["@dj-fiorex"]` |
| `MULTI_CONF` | ✅ | `False` — correct, only one mesh instance per device |
| `DEPENDENCIES` | ❌ Missing | The component implicitly depends on either `sx126x` or `sx127x`, but neither is declared. ESPHome expects explicit `DEPENDENCIES = [...]`. |
| `AUTO_LOAD` | ➖ | Not needed |
| `CONFLICTS_WITH` | ➖ | Not needed |

**Issue:** Omitting `DEPENDENCIES` means the component will silently compile even if the user hasn't added an `sx126x` or `sx127x` block. The error is only caught at code-generation time via a runtime `raise cv.Invalid(...)`, not at schema validation time.

---

## 3. Naming Conventions

### Python

| Convention | Status | Example |
|---|---|---|
| PEP 8 / snake_case | ✅ | `send_action_to_code`, `CONF_MESH_SECRET` |
| Module-level config keys as `CONF_*` constants | ✅ | All keys properly extracted |
| Namespace declared | ✅ | `lora_mesh_ns = cg.esphome_ns.namespace("lora_mesh")` |

### C++

| Convention | Status | Notes |
|---|---|---|
| `UpperCamelCase` for classes | ✅ | `LoraMesh`, `MeshMessage`, `RouteEntry`, `GatewayMode` |
| `lower_snake_case` for methods | ✅ | `send_message()`, `process_hello_()`, `find_route_()` |
| Trailing `_` on protected fields | ✅ | `node_id_`, `mesh_secret_`, `routes_`, `seen_cache_head_` |
| Trailing `_` on protected methods | ✅ | `build_hello_packet_()`, `transmit_()`, `alloc_route_slot_()` |
| `this->` prefix on all member accesses | ✅ | Consistent throughout |
| Line length ≤ 120 characters | ✅ | No violations observed |
| Two-space indentation | ✅ | Consistent |

**Minor note:** `LoRaRadio`, `LoRaSX126xRadio`, `LoRaSX127xRadio` preserve the `LoRa` acronym capitalisation. This is domain-consistent but slightly irregular under the Google C++ Style (which would prefer `LoraRadio`). Cosmetic concern only.

---

## 4. Field Visibility

| Rule | Status | Notes |
|---|---|---|
| Most fields `protected` | ✅ | All state is in `protected:` section |
| No safety-critical invariants requiring `private` | ✅ | No pointer-lifetime or sync invariants identified |
| `protected` accessor methods for derived classes | ➖ N/A | No derived classes planned |

---

## 5. `#define` Usage

| Rule | Status | Notes |
|---|---|---|
| No `#define` for ordinary constants | ✅ | Protocol constants use `static constexpr` |
| `#define` only for compile-time sizes from `cg.add_define()` | ✅ | `LORA_MESH_MAX_ROUTES`, `LORA_MESH_SEEN_CACHE_SIZE` — annotated with `// NOLINT` |
| Conditional compilation `#ifdef` guards | ✅ | `USE_SENSOR`, `USE_BINARY_SENSOR`, `USE_TEXT_SENSOR`, `USE_WIFI`, `LORA_MESH_USE_SX126X` |
| Fallback values documented | ✅ | Header comment explicitly states they are IDE/static-analysis-only fallbacks |

This area is **fully compliant**.

---

## 6. Configuration Validation

| Rule | Status | Notes |
|---|---|---|
| `cv.COMPONENT_SCHEMA` extended | ✅ | `.extend(cv.COMPONENT_SCHEMA)` |
| Typed validators on all keys | ✅ | `cv.int_range`, `cv.positive_time_period_milliseconds`, `cv.enum`, `cv.boolean`, `cv.string`, `cv.templatable` |
| `cv.Required()` / `cv.Optional()` with defaults | ✅ | Sensible defaults for all optional keys |
| `cv.use_id()` for cross-references | ✅ | Radio and all diagnostic sensors |
| Platform / framework constraints | ❌ Missing | No `cv.only_on()` or `cv.only_with_framework()`. The component depends on SX126x/SX127x LoRa hardware; ESP8266 and RP2040 support should be explicitly declared or excluded. |

---

## 7. Code Generation (`to_code`)

| Rule | Status | Notes |
|---|---|---|
| `cg.new_Pvariable()` + `cg.register_component()` | ✅ | Correct pattern |
| `cg.add_define()` for compile-time array sizes | ✅ | `LORA_MESH_MAX_ROUTES`, `LORA_MESH_SEEN_CACHE_SIZE` |
| `cg.templatable()` for templatable values | ✅ | `node_id`, `destination`, `payload` |
| `cg.register_parented()` for actions | ✅ | Used in all action `_to_code` functions |
| State management: no module-level mutable globals | ✅ | No module-level state |

### ⚠️ Non-Standard Radio Type Detection

The radio adapter selection uses a fragile pattern:

```python
try:
    from esphome.components.sx126x import SX126x  # noqa: PLC0415
    sx126x_type = SX126x
except ImportError:
    sx126x_type = None
```

Issues:
1. **Import at runtime inside `to_code`** violates the module-level import convention.
2. **Fragile** — breaks if the internal component class name or path changes.
3. **No schema-level check** — the incompatibility is only caught at code generation, not at YAML validation time.

The preferred approach is to declare `DEPENDENCIES`, or to expose a typed `use_id()` that validates the referenced component type via a custom schema validator.

### ⚠️ Non-Standard Include Injection

```python
cg.add_global(cg.RawExpression(
    '#include "esphome/components/lora_mesh/lora_radio_adapters.h"'
))
```

Using `cg.add_global(cg.RawExpression('#include "..."'))` to inject includes is unusual. The standard approach is to include headers directly from `lora_mesh.h` (conditionally via `#ifdef`), not inject them via code generation.

---

## 8. Automation System

### Triggers

| Rule | Status | Notes |
|---|---|---|
| `build_callback_automation()` used (preferred) | ✅ | All three triggers use this |
| No C++ Trigger classes (not needed) | ✅ | Correctly omitted |
| Callback methods templatized (`template<typename F>`) | ✅ | All three `add_on_*_callback()` methods |
| `CallbackManager` vs `LazyCallbackManager` chosen correctly | ✅ | `message_callback_` → `CallbackManager`; route/gateway → `LazyCallbackManager` |

### Actions

| Rule | Status | Notes |
|---|---|---|
| Action classes in `automation.h` | ✅ | Well-separated |
| Inherits `Action<Ts...>` and `Parented<LoraMesh>` | ✅ | Correct pattern |
| `TEMPLATABLE_VALUE` macro used | ✅ | `destination`, `payload` |
| `synchronous=True` specified | ✅ | All three actions |
| `@automation.register_action` decorator | ✅ | All three actions |

**The automation system is one of the strongest parts of this implementation.**

---

## 9. Memory and Embedded System Optimisation

### ✅ Good Practices

| Item | Notes |
|---|---|
| `std::array<RouteEntry, LORA_MESH_MAX_ROUTES>` | Fixed-size, no heap; compile-time size via `#define` |
| `std::array<SeenEntry, LORA_MESH_SEEN_CACHE_SIZE>` | Same — ring buffer implemented correctly |
| Ring buffer for seen cache (`seen_cache_head_`) | Efficient, no allocation after `setup()` |
| LRU eviction in `alloc_route_slot_()` | Correct: evicts worst (most hops, lowest RSSI) |
| `LazyCallbackManager` for rarely-registered callbacks | Saves 8 bytes per instance |
| `static_assert` to guard array bounds | `static_assert(LORA_MESH_MAX_ROUTES <= 255, ...)` |

### ❌ Heap Allocation Concerns

| Item | Impact | Notes |
|---|---|---|
| `std::string` fields in `MeshMessage` (`source`, `destination`, `prev_hop`, `payload`) | Medium | Every received DATA packet allocates 3–4 `std::string` heap objects. On high-traffic meshes this contributes to heap fragmentation. |
| `std::vector<uint8_t>` returned by `build_header_()`, `build_hello_packet_()`, `build_data_packet_()` | **High** | Every TX/RX cycle allocates and deallocates a `std::vector`. LoRa max payload is 255 bytes — a fixed-size buffer (`std::array<uint8_t, 255>` + length) would eliminate all per-packet heap allocation. |
| `std::string` returned by `id_to_hex()` | Low | Called frequently in logging and `MeshMessage` construction; can use a `char[9]` output parameter |
| `get_routing_table_json()` builds `std::string` | Low | Only called every 30 s for diagnostics; acceptable |
| `std::vector<uint8_t>` copy for packet forwarding in `process_data_()` | Medium | Every forwarded packet allocates a copy |

**Recommended fix:** Replace `std::vector<uint8_t>` packet buffers with `StaticVector<uint8_t, 255>` from `esphome/core/helpers.h`, or `std::array<uint8_t, 255>` + length counter.

### Container Usage Summary

| Container | Where | Compliant? |
|---|---|---|
| `std::array` | Route table, seen cache, local sort buffer | ✅ |
| `std::vector<uint8_t>` | All packet building/receiving | ❌ Should be fixed-size |
| `std::string` | `MeshMessage`, `id_to_hex`, diagnostic JSON | ⚠️ Hard to avoid in public API; `id_to_hex` could use a char buffer |
| `std::sort` | `build_hello_packet_()` — sorts route pointers | ✅ Operates on local `std::array`; no heap |

---

## 10. Constructor Parameters vs Setters

| Rule | Status | Notes |
|---|---|---|
| Required + invariant dependencies as constructor params | ⚠️ Partial | `LoRaSX126xRadio(sx126x::SX126x *radio)` and `LoRaSX127xRadio(sx127x::SX127x *radio)` correctly use constructor injection ✅. But `LoraMesh::set_radio(LoRaRadio *radio)` is a setter ❌ — `radio_` is required and never changes after `setup()`; it should be a constructor parameter. |

---

## 11. Protocol / Security

| Concern | Status | Notes |
|---|---|---|
| `mesh_secret` naming implies real security | ⚠️ | The secret is hashed (FNV-1a) into `mesh_id_` used as a network discriminator only. It is **not** encryption or authentication. Any sniffer who captures one packet learns the `mesh_id_`. The name `mesh_secret` is misleading — consider `mesh_network_key` with documentation stating it provides isolation, not cryptographic security. |
| Secret stored as plain string in firmware | Unavoidable | Common in ESPHome; acceptable. |
| No replay protection beyond TTL window | ⚠️ | The monotonic `seq_counter_` resets to 0 on reboot, allowing replay of old packets within the seen-cache TTL window. |
| FNV-1a is not a cryptographic hash | Noted | Expected for embedded use; collision probability is acceptable for mesh IDs. |

---

## 12. Tests

| Check | Status | Notes |
|---|---|---|
| `tests/components/lora_mesh/common.yaml` | ✅ Present | Tests node config, all automations, and all three actions |
| `tests/components/lora_mesh/test.esp32-idf.yaml` | ✅ Present | Uses `spi` package + `sx126x` radio; pins via substitutions |
| `tests/components/lora_mesh/test.esp32-ard.yaml` | ✅ Present | Same structure, Arduino framework |
| Shared SPI bus via package (`test_build_components/common/spi/`) | ✅ Correct | Follows the "never define buses directly in test YAML" rule |
| `common.yaml` has no bus definitions | ✅ Correct | Component-only config as required |
| `esphome/core/defines.h` entries | ❌ Missing | `LORA_MESH_USE_SX126X`, `LORA_MESH_USE_SX127X`, `LORA_MESH_MAX_ROUTES`, `LORA_MESH_SEEN_CACHE_SIZE` should be added to `defines.h` for IDE / static analysis support |
| Test coverage for `sx127x` radio variant | ❌ Missing | Only `sx126x` is tested; no `test.esp32-*.yaml` covering an `sx127x` radio |
| Test for `gateway: auto` mode | ❌ Missing | Auto-gateway (Wi-Fi detection) path is not exercised in tests |

---

## 13. Summary Table

| Area | Rating | Key Findings |
|---|---|---|
| File structure | ✅ Good | All required files present, logically split |
| Python naming | ✅ Good | Fully PEP 8 compliant |
| C++ naming | ✅ Good | Minor acronym capitalisation quirk (cosmetic) |
| Metadata | ⚠️ Fair | Missing `DEPENDENCIES` |
| Config validation | ⚠️ Fair | Missing platform constraints (`cv.only_on`) |
| Code generation | ⚠️ Fair | Non-standard dynamic import for radio detection; raw `#include` injection |
| Automation triggers | ✅ Excellent | Correct `build_callback_automation`, templatized callbacks, `LazyCallbackManager` |
| Actions | ✅ Excellent | Correct `Parented<>`, `TEMPLATABLE_VALUE`, `synchronous=True` |
| Memory / heap | ⚠️ Needs work | `std::vector<uint8_t>` for all packet buffers; `std::string` in hot path |
| Fixed-size containers | ✅ Good | Route table and seen cache use `std::array` correctly |
| Constructor vs setters | ⚠️ Fair | `LoraMesh::radio_` should be a constructor parameter |
| Security / protocol | ⚠️ Fair | `mesh_secret` is a network discriminator only; naming is misleading |
| Tests | ✅ Good | `common.yaml` + `esp32-idf` + `esp32-ard` present, correct bus-package pattern; missing `sx127x` and `gateway: auto` coverage, and `defines.h` entries |

---

## 14. Priority Recommendations

### High Priority

1. **Replace `std::vector<uint8_t>` packet buffers** with `StaticVector<uint8_t, 255>` or `std::array<uint8_t, 255>` + length counter to eliminate all per-packet heap allocations.
2. **Fix radio type detection** — declare `DEPENDENCIES` or use a proper typed schema validator instead of `try: from esphome.components.sx126x import SX126x` inside `to_code`.
3. **Add `defines.h` entries** for `LORA_MESH_USE_SX126X`, `LORA_MESH_USE_SX127X`, `LORA_MESH_MAX_ROUTES`, `LORA_MESH_SEEN_CACHE_SIZE`.

### Medium Priority

4. **Add platform constraints** — `cv.only_on(["esp32"])` (or appropriate set) to prevent compilation on unsupported platforms.
5. **Rename `mesh_secret` → `mesh_network_key`** — document clearly that it provides network isolation only, not cryptographic security.
6. **Add `sx127x` test variant** — `test.esp32-idf-sx127x.yaml` or equivalent to verify the second radio adapter.
7. **Add `gateway: auto` test case** — exercise the Wi-Fi-dependent gateway path.

### Low Priority

8. **Convert `LoraMesh::set_radio()` to constructor parameter** — aligns with the "required invariant = constructor param" rule.
9. **Replace `cg.add_global(cg.RawExpression('#include ...'))` pattern** — prefer conditional includes inside the component header.
10. **`id_to_hex()`** — return via `char[9]` output parameter to avoid allocating a `std::string`.
