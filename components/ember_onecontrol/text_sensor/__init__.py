import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID, CONF_TYPE
from .. import ember_onecontrol_ns, CONF_EMBER_ONECONTROL_ID, EmberOneControl

DEPENDENCIES = ["ember_onecontrol"]

EmberTextSensor = ember_onecontrol_ns.class_(
    "EmberTextSensor", text_sensor.TextSensor, cg.Component
)

EMBER_TEXT_SENSOR_TYPE_ns = ember_onecontrol_ns.namespace("EMBER_TEXT_SENSOR_TYPE")

CONF_SUPPORTED_TYPE = {
    "BATTERY_STATUS": EMBER_TEXT_SENSOR_TYPE_ns.BATTERY_STATUS,
    "CONNECTION_STATUS": EMBER_TEXT_SENSOR_TYPE_ns.CONNECTION_STATUS,
}

CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema(EmberTextSensor)
    .extend(
        {
            cv.GenerateID(CONF_EMBER_ONECONTROL_ID): cv.use_id(EmberOneControl),
            cv.Required(CONF_TYPE): cv.enum(CONF_SUPPORTED_TYPE, upper=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_EMBER_ONECONTROL_ID])

    cg.add(var.set_type(CONF_SUPPORTED_TYPE[config[CONF_TYPE]]))
