import pytest

from esphome import automation, config_validation as cv
from esphome.components.lora_mesh import CONFIG_SCHEMA

VALID_FABRIC_KEY = "00112233445566778899aabbccddeeff"


def _validate_lora_mesh_config(**overrides):
    config = {"radio_id": "mesh_radio", "fabric_key": VALID_FABRIC_KEY}
    config.update(overrides)
    return CONFIG_SCHEMA(config)


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
