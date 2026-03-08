import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID, CONF_TYPE
from .. import ember_onecontrol_ns, CONF_EMBER_ONECONTROL_ID, EmberOneControl

DEPENDENCIES = ["ember_onecontrol"]

EmberSwitch = ember_onecontrol_ns.class_("EmberSwitch", switch.Switch, cg.Component)

EMBER_SWITCH_TYPE_ns = ember_onecontrol_ns.namespace("EMBER_SWITCH_TYPE")

CONF_TABLE_ID = "table_id"
CONF_DEVICE_ID = "device_id"

CONF_SUPPORTED_TYPE = {
    "WATER_PUMP": EMBER_SWITCH_TYPE_ns.WATER_PUMP,
}

CONFIG_SCHEMA = (
    switch.switch_schema(EmberSwitch)
    .extend(
        {
            cv.GenerateID(CONF_EMBER_ONECONTROL_ID): cv.use_id(EmberOneControl),
            cv.Required(CONF_TYPE): cv.enum(CONF_SUPPORTED_TYPE, upper=True),
            cv.Optional(CONF_TABLE_ID): cv.hex_uint8_t,
            cv.Optional(CONF_DEVICE_ID): cv.hex_uint8_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await switch.register_switch(var, config)
    await cg.register_parented(var, config[CONF_EMBER_ONECONTROL_ID])

    cg.add(var.set_type(CONF_SUPPORTED_TYPE[config[CONF_TYPE]]))
    if CONF_TABLE_ID in config:
        cg.add(var.set_table_id(config[CONF_TABLE_ID]))
    if CONF_DEVICE_ID in config:
        cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
