from pathlib import Path

import pytest


@pytest.mark.parametrize(
    ("fixture_name", "adapter_type"),
    (
        ("codegen_sx126x.yaml", "LoRaSX126xRadio"),
        ("codegen_sx127x.yaml", "LoRaSX127xRadio"),
    ),
)
def test_selected_radio_adapter_is_constructed_before_mesh_and_injected(
    generate_main, fixture_name: str, adapter_type: str
) -> None:
    main_cpp = generate_main(Path(__file__).with_name(fixture_name))

    adapter_creation = f"new(mesh_radio_adapter) lora_mesh::{adapter_type}(test_radio);"
    mesh_creation = (
        "new(mesh) lora_mesh::LoraMesh("
        '"00000000000000000000000000000000", mesh_radio_adapter);'
    )

    assert adapter_creation in main_cpp
    assert mesh_creation in main_cpp
    assert main_cpp.index(adapter_creation) < main_cpp.index(mesh_creation)
    assert "mesh->set_radio(" not in main_cpp
