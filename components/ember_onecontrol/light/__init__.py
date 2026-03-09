import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_ID, CONF_TYPE, CONF_OUTPUT_ID
from .. import ember_onecontrol_ns, CONF_EMBER_ONECONTROL_ID, EmberOneControl

DEPENDENCIES = ["ember_onecontrol"]

EmberLight = ember_onecontrol_ns.class_("EmberLight", light.LightOutput, cg.Component)

EMBER_LIGHT_TYPE_ns = ember_onecontrol_ns.namespace("EMBER_LIGHT_TYPE")

CONF_TABLE_ID = "table_id"
CONF_DEVICE_ID = "device_id"

CONF_SUPPORTED_TYPE = {
    "ACCENT_LIGHT": EMBER_LIGHT_TYPE_ns.ACCENT_LIGHT,
    "STEP_LIGHT": EMBER_LIGHT_TYPE_ns.STEP_LIGHT,
    "PUMP_LIGHT": EMBER_LIGHT_TYPE_ns.PUMP_LIGHT,
    "AWNING_LIGHT": EMBER_LIGHT_TYPE_ns.AWNING_LIGHT,
    "CEILING_LIGHT": EMBER_LIGHT_TYPE_ns.CEILING_LIGHT,
}

CONFIG_SCHEMA = (
    light.LIGHT_SCHEMA.extend(
        {
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(EmberLight),
            cv.GenerateID(CONF_EMBER_ONECONTROL_ID): cv.use_id(EmberOneControl),
            cv.Required(CONF_TYPE): cv.enum(CONF_SUPPORTED_TYPE, upper=True),
            cv.Optional(CONF_TABLE_ID): cv.hex_uint8_t,
            cv.Optional(CONF_DEVICE_ID): cv.hex_uint8_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)
    await cg.register_parented(var, config[CONF_EMBER_ONECONTROL_ID])

    cg.add(var.set_type(CONF_SUPPORTED_TYPE[config[CONF_TYPE]]))
    if CONF_TABLE_ID in config:
        cg.add(var.set_table_id(config[CONF_TABLE_ID]))
    if CONF_DEVICE_ID in config:
        cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
