"""ESPHome lora_mesh component — LoRa multi-hop mesh networking."""

from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.components.sx126x import SX126x
from esphome.components.sx127x import SX127x
import esphome.config_validation as cv
from esphome.const import CONF_GATEWAY, CONF_ID, CONF_ON_MESSAGE, CONF_PAYLOAD
from esphome.core import ID

CODEOWNERS = ["@dj-fiorex"]
MULTI_CONF = False

# ── Config keys ────────────────────────────────────────────────────────────────
CONF_RADIO_ID = "radio_id"
CONF_NODE_ID = "node_id"
CONF_MESH_SECRET = "mesh_secret"
CONF_MAX_HOPS = "max_hops"
CONF_DISCOVERY_INTERVAL = "discovery_interval"
CONF_ROUTE_TTL = "route_ttl"
CONF_MAX_ROUTES = "max_routes"
CONF_SEEN_CACHE_SIZE = "seen_cache_size"
CONF_SEEN_CACHE_TTL = "seen_cache_ttl"
CONF_FORWARD_MESSAGES = "forward_messages"
CONF_DESTINATION = "destination"

CONF_ON_ROUTE_UPDATE = "on_route_update"
CONF_ON_GATEWAY_CHANGED = "on_gateway_changed"

CONF_NODE_COUNT_SENSOR_ID = "node_count_sensor_id"
CONF_GATEWAY_AVAILABLE_SENSOR_ID = "gateway_available_sensor_id"
CONF_ROUTING_TABLE_SENSOR_ID = "routing_table_sensor_id"
CONF_BEST_GATEWAY_SENSOR_ID = "best_gateway_sensor_id"

# ── C++ types ──────────────────────────────────────────────────────────────────
lora_mesh_ns = cg.esphome_ns.namespace("lora_mesh")

LoraMesh = lora_mesh_ns.class_("LoraMesh", cg.Component)
LoRaRadio = lora_mesh_ns.class_("LoRaRadio")
GatewayMode = lora_mesh_ns.enum("GatewayMode", is_class=True)
MeshMessage = lora_mesh_ns.class_("MeshMessage")

# Radio adapter C++ types (conditionally used based on radio_id type)
LoRaSX126xRadio = lora_mesh_ns.class_("LoRaSX126xRadio", LoRaRadio)
LoRaSX127xRadio = lora_mesh_ns.class_("LoRaSX127xRadio", LoRaRadio)

# Actions
SendMessageAction = lora_mesh_ns.class_(
    "SendMessageAction", automation.Action, cg.Parented.template(LoraMesh)
)
BroadcastMessageAction = lora_mesh_ns.class_(
    "BroadcastMessageAction", automation.Action, cg.Parented.template(LoraMesh)
)
SendToGatewayAction = lora_mesh_ns.class_(
    "SendToGatewayAction", automation.Action, cg.Parented.template(LoraMesh)
)

GATEWAY_MODES = {
    "normal": GatewayMode.NORMAL,
    "gateway": GatewayMode.GATEWAY,
    "auto": GatewayMode.AUTO,
}

# ── Configuration schema ───────────────────────────────────────────────────────

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LoraMesh),
            # Radio reference — must be an sx126x or sx127x component ID.
            cv.Required(CONF_RADIO_ID): cv.Any(
                cv.use_id(SX126x),
                cv.use_id(SX127x),
            ),
            # Node / mesh identity
            cv.Optional(CONF_NODE_ID): cv.templatable(cv.string),
            cv.Required(CONF_MESH_SECRET): cv.string,
            # Gateway behaviour
            cv.Optional(CONF_GATEWAY, default="normal"): cv.enum(
                GATEWAY_MODES, lower=True
            ),
            # Routing parameters
            cv.Optional(CONF_MAX_HOPS, default=8): cv.int_range(min=1, max=255),
            cv.Optional(
                CONF_DISCOVERY_INTERVAL, default="30s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_ROUTE_TTL, default="5min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MAX_ROUTES, default=16): cv.int_range(min=4, max=255),
            cv.Optional(CONF_SEEN_CACHE_SIZE, default=32): cv.int_range(
                min=8, max=255
            ),
            cv.Optional(
                CONF_SEEN_CACHE_TTL, default="2min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_FORWARD_MESSAGES, default=True): cv.boolean,
            # Automation triggers
            cv.Optional(CONF_ON_MESSAGE): automation.validate_automation({}),
            cv.Optional(CONF_ON_ROUTE_UPDATE): automation.validate_automation({}),
            cv.Optional(CONF_ON_GATEWAY_CHANGED): automation.validate_automation({}),
            # Optional diagnostic sensors (user must declare these separately)
            cv.Optional(CONF_NODE_COUNT_SENSOR_ID): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_GATEWAY_AVAILABLE_SENSOR_ID): cv.use_id(
                binary_sensor.BinarySensor
            ),
            cv.Optional(CONF_ROUTING_TABLE_SENSOR_ID): cv.use_id(
                text_sensor.TextSensor
            ),
            cv.Optional(CONF_BEST_GATEWAY_SENSOR_ID): cv.use_id(
                text_sensor.TextSensor
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA)
)

# ── Code generation ────────────────────────────────────────────────────────────


async def to_code(config) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Compile-time array sizes via preprocessor defines.
    cg.add_define("LORA_MESH_MAX_ROUTES", config[CONF_MAX_ROUTES])
    cg.add_define("LORA_MESH_SEEN_CACHE_SIZE", config[CONF_SEEN_CACHE_SIZE])

    # ── Radio adapter ──────────────────────────────────────────────────────
    radio_var = await cg.get_variable(config[CONF_RADIO_ID])

    radio_type = getattr(radio_var, "type", None)

    if radio_type == SX126x:
        cg.add_define("LORA_MESH_USE_SX126X")
        cg.add_global(
            cg.RawExpression(
                '#include "esphome/components/lora_mesh/lora_radio_adapters.h"'
            )
        )
        adapter_id = ID(
            f"{config[CONF_ID].id}_radio_adapter",
            is_declaration=True,
            type=LoRaSX126xRadio,
        )
        adapter = cg.new_Pvariable(adapter_id, radio_var)
        cg.add(var.set_radio(adapter))

    elif radio_type == SX127x:
        cg.add_define("LORA_MESH_USE_SX127X")
        cg.add_global(
            cg.RawExpression(
                '#include "esphome/components/lora_mesh/lora_radio_adapters.h"'
            )
        )
        adapter_id = ID(
            f"{config[CONF_ID].id}_radio_adapter",
            is_declaration=True,
            type=LoRaSX127xRadio,
        )
        adapter = cg.new_Pvariable(adapter_id, radio_var)
        cg.add(var.set_radio(adapter))

    else:
        raise cv.Invalid(
            f"radio_id must reference an sx126x or sx127x component "
            f"(got type: {radio_type})"
        )

    # ── Configuration setters ─────────────────────────────────────────────
    if CONF_NODE_ID in config:
        node_id_templ = await cg.templatable(config[CONF_NODE_ID], [], cg.std_string)
        cg.add(var.set_node_id(node_id_templ))
    cg.add(var.set_mesh_secret(config[CONF_MESH_SECRET]))
    cg.add(var.set_gateway_mode(config[CONF_GATEWAY]))
    cg.add(var.set_max_hops(config[CONF_MAX_HOPS]))
    cg.add(var.set_discovery_interval(int(config[CONF_DISCOVERY_INTERVAL].total_milliseconds)))
    cg.add(var.set_route_ttl(int(config[CONF_ROUTE_TTL].total_milliseconds)))
    cg.add(var.set_seen_cache_ttl(int(config[CONF_SEEN_CACHE_TTL].total_milliseconds)))
    cg.add(var.set_forward_messages(config[CONF_FORWARD_MESSAGES]))

    # ── on_message trigger — passes MeshMessage by value as 'x' ──────────
    for conf in config.get(CONF_ON_MESSAGE, []):
        await automation.build_callback_automation(
            var,
            "add_on_message_callback",
            [(MeshMessage, "x")],
            conf,
        )

    # ── on_route_update trigger ───────────────────────────────────────────
    for conf in config.get(CONF_ON_ROUTE_UPDATE, []):
        await automation.build_callback_automation(
            var,
            "add_on_route_update_callback",
            [],
            conf,
        )

    # ── on_gateway_changed trigger ────────────────────────────────────────
    for conf in config.get(CONF_ON_GATEWAY_CHANGED, []):
        await automation.build_callback_automation(
            var,
            "add_on_gateway_changed_callback",
            [],
            conf,
        )

    # ── Optional diagnostic sensors ───────────────────────────────────────
    if CONF_NODE_COUNT_SENSOR_ID in config:
        sens = await cg.get_variable(config[CONF_NODE_COUNT_SENSOR_ID])
        cg.add(var.set_node_count_sensor(sens))

    if CONF_GATEWAY_AVAILABLE_SENSOR_ID in config:
        bs = await cg.get_variable(config[CONF_GATEWAY_AVAILABLE_SENSOR_ID])
        cg.add(var.set_gateway_available_sensor(bs))

    if CONF_ROUTING_TABLE_SENSOR_ID in config:
        ts = await cg.get_variable(config[CONF_ROUTING_TABLE_SENSOR_ID])
        cg.add(var.set_routing_table_sensor(ts))

    if CONF_BEST_GATEWAY_SENSOR_ID in config:
        ts = await cg.get_variable(config[CONF_BEST_GATEWAY_SENSOR_ID])
        cg.add(var.set_best_gateway_sensor(ts))


# ── Action schemas and registrations ─────────────────────────────────────────

SEND_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(LoraMesh),
        cv.Required(CONF_DESTINATION): cv.templatable(cv.string),
        cv.Required(CONF_PAYLOAD): cv.templatable(cv.string),
    }
)

PAYLOAD_ONLY_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(LoraMesh),
        cv.Required(CONF_PAYLOAD): cv.templatable(cv.string),
    }
)


@automation.register_action(
    "lora_mesh.send",
    SendMessageAction,
    SEND_ACTION_SCHEMA,
    synchronous=True,
)
async def send_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    templ = await cg.templatable(config[CONF_DESTINATION], args, cg.std_string)
    cg.add(var.set_destination(templ))
    templ = await cg.templatable(config[CONF_PAYLOAD], args, cg.std_string)
    cg.add(var.set_payload(templ))
    return var


@automation.register_action(
    "lora_mesh.broadcast",
    BroadcastMessageAction,
    PAYLOAD_ONLY_ACTION_SCHEMA,
    synchronous=True,
)
async def broadcast_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    templ = await cg.templatable(config[CONF_PAYLOAD], args, cg.std_string)
    cg.add(var.set_payload(templ))
    return var


@automation.register_action(
    "lora_mesh.send_to_gateway",
    SendToGatewayAction,
    PAYLOAD_ONLY_ACTION_SCHEMA,
    synchronous=True,
)
async def send_to_gateway_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    templ = await cg.templatable(config[CONF_PAYLOAD], args, cg.std_string)
    cg.add(var.set_payload(templ))
    return var
