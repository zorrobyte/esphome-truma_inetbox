import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble_tracker, esp32_ble
from esphome.components.esp32 import add_idf_sdkconfig_option
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
    esp32_ble.GATTcEventHandler,
    esp32_ble.GAPEventHandler,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EmberOneControl),
            cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
            cv.Optional(CONF_PIN, default="357694"): cv.string,
        }
    )
    .extend(cv.polling_component_schema("30s"))
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    # Enable BLE SMP bonding with NVS persistence (required for bond keys to survive reboot/OTA)
    add_idf_sdkconfig_option("CONFIG_BT_SMP_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLE_SMP_BOND_NVS_FLASH", True)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)
    # Register as GATTc event handler so we receive GATT client events
    ble = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
    esp32_ble.register_gattc_event_handler(ble, var)
    esp32_ble.register_gap_event_handler(ble, var)
    cg.add(var.set_pin(config[CONF_PIN]))
