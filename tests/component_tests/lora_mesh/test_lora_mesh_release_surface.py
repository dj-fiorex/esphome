from pathlib import Path

from script.helpers import get_component_test_files, parse_test_filename

COMPILE_FIXTURE_PATH = Path(
    "tests/components/lora_mesh/test-three-pot.esp32-s3-idf.yaml"
)
SCENARIO_PATH = Path("tests/components/lora_mesh/three-pot.esp32-s3-idf.yaml")
SCENARIO_SECRETS_EXAMPLE_PATH = SCENARIO_PATH.parent / "secrets.example.yaml"
THREE_POT_GUIDE_PATH = Path("esphome/components/lora_mesh/docs/three-pot-scenario.md")
WIRE_FORMAT_PATH = Path("esphome/components/lora_mesh/docs/wire-format.md")


def test_three_pot_scenario_is_an_esp32_idf_compile_fixture() -> None:
    component_tests = get_component_test_files("lora_mesh", all_variants=True)

    assert COMPILE_FIXTURE_PATH.resolve() in {
        path.resolve() for path in component_tests
    }
    assert parse_test_filename(COMPILE_FIXTURE_PATH) == (
        "test-three-pot",
        "esp32-s3-idf",
    )


def test_three_pot_scenario_resolves_a_non_secret_test_key_via_secrets() -> None:
    scenario = SCENARIO_PATH.read_text(encoding="utf-8")
    secrets_example = SCENARIO_SECRETS_EXAMPLE_PATH.read_text(encoding="utf-8")

    assert "fabric_key: !secret lora_mesh_fabric_key" in scenario
    assert 'lora_mesh_fabric_key: "00000000000000000000000000000000"' in secrets_example
    assert 'fabric_key: "00000000000000000000000000000000"' not in scenario


def test_three_pot_artifacts_use_only_the_protocol_v4_public_surface() -> None:
    scenario = SCENARIO_PATH.read_text(encoding="utf-8")
    compile_fixture = COMPILE_FIXTURE_PATH.read_text(encoding="utf-8")
    guide = THREE_POT_GUIDE_PATH.read_text(encoding="utf-8")
    active_artifacts = f"{scenario}\n{compile_fixture}\n{guide}"

    for removed_surface in (
        "mesh_secret:",
        "group_key:",
        "best_gateway_sensor_id:",
        "on_gateway_changed:",
    ):
        assert removed_surface not in active_artifacts

    assert "fabric_key: !secret" in scenario
    for required_surface in (
        "nearest_gateway_sensor_id:",
        "lora_mesh.set_upstream_connected:",
        "lora_mesh.send_to_gateway:",
    ):
        assert required_surface in compile_fixture


def test_three_pot_guide_covers_the_application_integration_contract() -> None:
    guide = THREE_POT_GUIDE_PATH.read_text(encoding="utf-8")

    for required_guidance in (
        "Node A → forwarding Node B → Gateway Node C",
        "publishes directly upstream",
        "buffering and retry",
        "source Node and the receiving Gateway",
        "named unicast",
        "no MQTT knowledge",
        "with `h1`",
    ):
        assert required_guidance in guide


def test_wire_format_describes_final_protocol_v4_hop_and_flag_semantics() -> None:
    wire_format = WIRE_FORMAT_PATH.read_text(encoding="utf-8")

    assert "origin had Upstream Connectivity at send time" in wire_format
    assert "number of Forwarding Nodes already traversed" in wire_format
    assert "`0x04` through `0x80` are reserved" in wire_format
    assert "`0x08` forwarded" not in wire_format
