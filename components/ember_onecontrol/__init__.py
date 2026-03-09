import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import CONF_ID

CODEOWNERS = ["@zorro"]

CONF_EMBER_ONECONTROL_ID = "ember_onecontrol_id"
CONF_PIN = "pin"

ember_onecontrol_ns = cg.esphome_ns.namespace("ember_onecontrol")
EmberOneControl = ember_onecontrol_ns.class_(
    "EmberOneControl",
    cg.PollingComponent,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EmberOneControl),
            cv.Optional(CONF_PIN, default="357694"): cv.string,
        }
    )
    .extend(cv.polling_component_schema("30s"))
)


async def to_code(config):
    # Switch from Bluedroid to NimBLE
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", True)

    # NimBLE role config — we are a central (GATT client) that scans
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ROLE_CENTRAL", True)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ROLE_OBSERVER", True)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ROLE_PERIPHERAL", False)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ROLE_BROADCASTER", False)

    # Security / bonding
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_SECURITY_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_SM_LEGACY", True)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_SM_SC", True)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_NVS_PERSIST", True)

    # Memory tuning
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_MAX_CONNECTIONS", 1)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_MAX_BONDS", 3)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pin(config[CONF_PIN]))
