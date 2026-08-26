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
