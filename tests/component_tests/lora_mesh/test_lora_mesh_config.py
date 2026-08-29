import pytest

from esphome import automation, config_validation as cv
from esphome.components.lora_mesh import CONFIG_SCHEMA, FINAL_VALIDATE_SCHEMA
from esphome.config import Config
from esphome.const import (
    CONF_FILTERS,
    CONF_ID,
    KEY_CORE,
    KEY_TARGET_PLATFORM,
    Platform,
    PlatformFramework,
)
from esphome.core import CORE, ID
import esphome.final_validate as fv
from tests.component_tests.types import SetCoreConfigCallable

VALID_FABRIC_KEY = "00112233445566778899aabbccddeeff"


@pytest.fixture(autouse=True)
def _target_esp32_idf(set_core_config: SetCoreConfigCallable) -> None:
    set_core_config(PlatformFramework.ESP32_IDF)


def _validate_lora_mesh_config(**overrides):
    config = {"radio_id": "mesh_radio", "fabric_key": VALID_FABRIC_KEY}
    config.update(overrides)
    return CONFIG_SCHEMA(config)


@pytest.mark.parametrize(
    "platform_framework",
    (PlatformFramework.ESP32_ARDUINO, PlatformFramework.ESP32_IDF),
)
def test_lora_mesh_accepts_supported_esp32_frameworks(
    platform_framework: PlatformFramework,
    set_core_config: SetCoreConfigCallable,
) -> None:
    set_core_config(platform_framework)

    _validate_lora_mesh_config()


@pytest.mark.parametrize(
    "platform",
    tuple(platform for platform in Platform if platform is not Platform.ESP32),
)
def test_lora_mesh_rejects_platforms_without_embedded_ccm_support(
    platform: Platform,
) -> None:
    CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = platform.value

    with pytest.raises(cv.Invalid, match="only available on"):
        _validate_lora_mesh_config()


def test_fabric_key_is_mandatory() -> None:
    with pytest.raises(cv.Invalid, match="fabric_key"):
        CONFIG_SCHEMA({"radio_id": "mesh_radio"})


@pytest.mark.parametrize(
    "fabric_key",
    (
        "",
        "00112233445566778899aabbccddee",
        "00112233445566778899aabbccddeeff00",
        "00112233445566778899aabbccddeefg",
    ),
)
def test_fabric_key_must_be_exactly_128_bits_of_hex(fabric_key: str) -> None:
    with pytest.raises(cv.Invalid, match="fabric_key must be 32 hex characters"):
        _validate_lora_mesh_config(fabric_key=fabric_key)


def test_fabric_key_accepts_exactly_128_bits_of_hex() -> None:
    config = _validate_lora_mesh_config()

    assert config["fabric_key"] == VALID_FABRIC_KEY


@pytest.mark.parametrize("removed_key", ("mesh_secret", "group_key"))
def test_removed_security_options_are_rejected(removed_key: str) -> None:
    with pytest.raises(cv.Invalid, match="extra keys not allowed"):
        _validate_lora_mesh_config(**{removed_key: "legacy-value"})


def test_runtime_group_key_action_is_removed() -> None:
    assert "lora_mesh.set_group_key" not in automation.ACTION_REGISTRY


@pytest.mark.parametrize(
    "removed_key", ("gateway", "on_gateway_changed", "best_gateway_sensor_id")
)
def test_removed_gateway_configuration_is_rejected(removed_key: str) -> None:
    with pytest.raises(cv.Invalid, match="extra keys not allowed"):
        _validate_lora_mesh_config(**{removed_key: "gateway"})


def test_upstream_connectivity_action_replaces_gateway_modes() -> None:
    assert "lora_mesh.set_upstream_connected" in automation.ACTION_REGISTRY


def test_nearest_gateway_diagnostic_uses_canonical_name() -> None:
    config = _validate_lora_mesh_config(nearest_gateway_sensor_id="nearest_gateway")

    assert config["nearest_gateway_sensor_id"].id == "nearest_gateway"


def test_routing_table_diagnostic_rejects_text_sensor_filters() -> None:
    config = _validate_lora_mesh_config(routing_table_sensor_id="routing_table")
    routing_table_id = ID("routing_table", is_declaration=True)
    full_config = Config()
    full_config["text_sensor"] = [
        {CONF_ID: routing_table_id, CONF_FILTERS: [{"to_upper": {}}]}
    ]
    full_config.declare_ids.append((routing_table_id, ["text_sensor", 0, CONF_ID]))
    token = fv.full_config.set(full_config)
    try:
        with pytest.raises(cv.Invalid, match="does not support text sensor filters"):
            FINAL_VALIDATE_SCHEMA(config)
    finally:
        fv.full_config.reset(token)


@pytest.mark.parametrize("discovery_interval", ("0ms", "1ms", "4ms"))
def test_discovery_interval_rejects_values_unsafe_for_setup_jitter(
    discovery_interval: str,
) -> None:
    with pytest.raises(cv.Invalid):
        _validate_lora_mesh_config(discovery_interval=discovery_interval)


def test_discovery_interval_accepts_minimum_safe_setup_jitter_value() -> None:
    config = _validate_lora_mesh_config(discovery_interval="5ms")

    assert config["discovery_interval"].total_milliseconds == 5


def test_discovery_interval_defaults_to_thirty_seconds() -> None:
    config = _validate_lora_mesh_config()

    assert config["discovery_interval"].total_milliseconds == 30_000


def test_route_ttl_accepts_largest_wrap_safe_hold_down_duration() -> None:
    config = _validate_lora_mesh_config(route_ttl="2147473647ms")

    assert config["route_ttl"].total_milliseconds == 2_147_473_647


def test_route_ttl_rejects_duration_that_makes_hold_down_ambiguous() -> None:
    with pytest.raises(cv.Invalid):
        _validate_lora_mesh_config(route_ttl="2147473648ms")


def test_seen_cache_ttl_accepts_largest_wrap_safe_duration() -> None:
    config = _validate_lora_mesh_config(seen_cache_ttl="2147483647ms")

    assert config["seen_cache_ttl"].total_milliseconds == 2_147_483_647


def test_seen_cache_ttl_rejects_ambiguous_signed_deadline_duration() -> None:
    with pytest.raises(cv.Invalid):
        _validate_lora_mesh_config(seen_cache_ttl="2147483648ms")


def test_tx_jitter_accepts_largest_wrap_safe_duration() -> None:
    config = _validate_lora_mesh_config(tx_jitter="2147483647ms")

    assert config["tx_jitter"].total_milliseconds == 2_147_483_647


def test_tx_jitter_rejects_ambiguous_signed_deadline_duration() -> None:
    with pytest.raises(cv.Invalid):
        _validate_lora_mesh_config(tx_jitter="2147483648ms")


def test_max_hops_rejects_route_length_that_cannot_be_advertised() -> None:
    with pytest.raises(cv.Invalid):
        _validate_lora_mesh_config(max_hops=255)


@pytest.mark.parametrize("max_hops", (1, 254))
def test_max_hops_accepts_advertisable_route_length_edges(max_hops: int) -> None:
    config = _validate_lora_mesh_config(max_hops=max_hops)

    assert config["max_hops"] == max_hops
