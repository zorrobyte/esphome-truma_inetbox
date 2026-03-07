import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID
from .. import ember_onecontrol_ns, CONF_EMBER_ONECONTROL_ID, EmberOneControl

DEPENDENCIES = ["ember_onecontrol"]

EmberPairingButton = ember_onecontrol_ns.class_(
    "EmberPairingButton", button.Button, cg.Component
)

CONFIG_SCHEMA = (
    button.button_schema(EmberPairingButton, icon="mdi:bluetooth-connect")
    .extend(
        {
            cv.GenerateID(CONF_EMBER_ONECONTROL_ID): cv.use_id(EmberOneControl),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_EMBER_ONECONTROL_ID])
