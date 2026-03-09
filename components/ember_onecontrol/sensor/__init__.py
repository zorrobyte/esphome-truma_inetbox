import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    CONF_TYPE,
    CONF_UNIT_OF_MEASUREMENT,
    CONF_ICON,
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    DEVICE_CLASS_VOLTAGE,
    UNIT_VOLT,
    UNIT_PERCENT,
    STATE_CLASS_MEASUREMENT,
)
from .. import ember_onecontrol_ns, CONF_EMBER_ONECONTROL_ID, EmberOneControl

DEPENDENCIES = ["ember_onecontrol"]

CONF_TABLE_ID = "table_id"
CONF_DEVICE_ID = "device_id"

EmberSensor = ember_onecontrol_ns.class_(
    "EmberSensor", sensor.Sensor, cg.Component
)

EMBER_SENSOR_TYPE_ns = ember_onecontrol_ns.namespace("EMBER_SENSOR_TYPE")

CONF_SUPPORTED_TYPE = {
    "BATTERY_VOLTAGE": {
        "class": EMBER_SENSOR_TYPE_ns.BATTERY_VOLTAGE,
        CONF_UNIT_OF_MEASUREMENT: UNIT_VOLT,
        CONF_ICON: "mdi:battery",
        CONF_ACCURACY_DECIMALS: 2,
        CONF_DEVICE_CLASS: DEVICE_CLASS_VOLTAGE,
    },
    "TANK_FRESH": {
        "class": EMBER_SENSOR_TYPE_ns.TANK_FRESH,
        CONF_UNIT_OF_MEASUREMENT: UNIT_PERCENT,
        CONF_ICON: "mdi:water",
        CONF_ACCURACY_DECIMALS: 0,
    },
    "TANK_BLACK": {
        "class": EMBER_SENSOR_TYPE_ns.TANK_BLACK,
        CONF_UNIT_OF_MEASUREMENT: UNIT_PERCENT,
        CONF_ICON: "mdi:water-opacity",
        CONF_ACCURACY_DECIMALS: 0,
    },
    "TANK_GREY": {
        "class": EMBER_SENSOR_TYPE_ns.TANK_GREY,
        CONF_UNIT_OF_MEASUREMENT: UNIT_PERCENT,
        CONF_ICON: "mdi:water-percent",
        CONF_ACCURACY_DECIMALS: 0,
    },
}


def set_default_based_on_type():
    def set_defaults_(config):
        sensor_type = CONF_SUPPORTED_TYPE[config[CONF_TYPE]]
        if CONF_UNIT_OF_MEASUREMENT in sensor_type and CONF_UNIT_OF_MEASUREMENT not in config:
            config[CONF_UNIT_OF_MEASUREMENT] = sensor_type[CONF_UNIT_OF_MEASUREMENT]
        if CONF_ICON in sensor_type and CONF_ICON not in config:
            config[CONF_ICON] = sensor_type[CONF_ICON]
        if CONF_ACCURACY_DECIMALS in sensor_type and CONF_ACCURACY_DECIMALS not in config:
            config[CONF_ACCURACY_DECIMALS] = sensor_type[CONF_ACCURACY_DECIMALS]
        if CONF_DEVICE_CLASS in sensor_type and CONF_DEVICE_CLASS not in config:
            config[CONF_DEVICE_CLASS] = sensor_type[CONF_DEVICE_CLASS]
        return config

    return set_defaults_


CONFIG_SCHEMA = (
    sensor.sensor_schema(state_class=STATE_CLASS_MEASUREMENT)
    .extend(
        {
            cv.GenerateID(): cv.declare_id(EmberSensor),
            cv.GenerateID(CONF_EMBER_ONECONTROL_ID): cv.use_id(EmberOneControl),
            cv.Required(CONF_TYPE): cv.enum(CONF_SUPPORTED_TYPE, upper=True),
            cv.Optional(CONF_TABLE_ID): cv.hex_uint8_t,
            cv.Optional(CONF_DEVICE_ID): cv.hex_uint8_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)
FINAL_VALIDATE_SCHEMA = set_default_based_on_type()


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)
    await cg.register_parented(var, config[CONF_EMBER_ONECONTROL_ID])

    cg.add(var.set_type(CONF_SUPPORTED_TYPE[config[CONF_TYPE]]["class"]))
    if CONF_TABLE_ID in config:
        cg.add(var.set_table_id(config[CONF_TABLE_ID]))
    if CONF_DEVICE_ID in config:
        cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
