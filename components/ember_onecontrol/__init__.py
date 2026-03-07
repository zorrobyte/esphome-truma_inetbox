import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble_tracker
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32_ble_tracker"]
CODEOWNERS = ["@zorro"]

CONF_EMBER_ONECONTROL_ID = "ember_onecontrol_id"
CONF_PIN = "pin"

ember_onecontrol_ns = cg.esphome_ns.namespace("ember_onecontrol")
EmberOneControl = ember_onecontrol_ns.class_(
    "EmberOneControl",
    cg.PollingComponent,
    esp32_ble_tracker.ESPBTDeviceListener,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EmberOneControl),
            cv.Optional(CONF_PIN, default="090336"): cv.string,
        }
    )
    .extend(cv.polling_component_schema("30s"))
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)
    cg.add(var.set_pin(config[CONF_PIN]))
