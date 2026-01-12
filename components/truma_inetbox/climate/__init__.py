from esphome.components import climate
import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.const import (
    CONF_ID,
    CONF_TYPE,
    CONF_NAME,
    CONF_VISUAL,
    CONF_TARGET_TEMPERATURE,
    CONF_MIN_TEMPERATURE,
    CONF_MAX_TEMPERATURE,
    CONF_TEMPERATURE_STEP,
)
from esphome.components.climate import (
    ClimateMode,
)

CLIMATE_MODES = {
    "OFF": ClimateMode.CLIMATE_MODE_OFF,
    "HEAT": ClimateMode.CLIMATE_MODE_HEAT,
    "AUTO": ClimateMode.CLIMATE_MODE_AUTO,
    "COOL": ClimateMode.CLIMATE_MODE_COOL,
    "HEAT_COOL": ClimateMode.CLIMATE_MODE_HEAT_COOL,
    "FAN_ONLY": ClimateMode.CLIMATE_MODE_FAN_ONLY,
}
CLIMATE_VISUAL_SCHEMA = cv.Schema({
    cv.Optional(CONF_TARGET_TEMPERATURE, default={}): cv.Schema({
        cv.Optional(CONF_MIN_TEMPERATURE, default=5.0): cv.float_,
        cv.Optional(CONF_MAX_TEMPERATURE, default=30.0): cv.float_,
        cv.Optional(CONF_TEMPERATURE_STEP, default=0.5): cv.float_,
    })
})
from .. import truma_inetbox_ns, CONF_TRUMA_INETBOX_ID, TrumaINetBoxApp

DEPENDENCIES = ["truma_inetbox"]
CODEOWNERS = ["@Fabian-Schmidt"]

TrumaClimate = truma_inetbox_ns.class_(
    "TrumaClimate", climate.Climate, cg.Component)

CONF_SUPPORTED_TYPE = {
    "ROOM": truma_inetbox_ns.class_("TrumaRoomClimate", climate.Climate, cg.Component),
    "WATER": truma_inetbox_ns.class_("TrumaWaterClimate", climate.Climate, cg.Component),
    "AIRCON": truma_inetbox_ns.class_("TrumaAirconClimate", climate.Climate, cg.Component),
}


CONF_TYPE_DEFAULTS = {
    "ROOM": {
        "supported_modes": ["OFF", "HEAT"],
        "name": "Truma Room Climate",
    },
    "WATER": {
        "supported_modes": ["OFF", "HEAT"],
        "name": "Truma Water Climate",
    },
    "AIRCON": {
        "supported_modes": ["OFF", "COOL", "HEAT", "HEAT_COOL", "FAN_ONLY"],
        "name": "Truma Aircon Climate",
    },
}


def set_default_based_on_type():
    def set_defaults_(config):
        # update the class
        type_key = config[CONF_TYPE].upper()
        config[CONF_ID].type = CONF_SUPPORTED_TYPE[type_key]

        # Set type-specific defaults if not already set
        type_defaults = CONF_TYPE_DEFAULTS.get(type_key, {})
        if "supported_modes" not in config or config["supported_modes"] == ["OFF", "HEAT"]:
            if "supported_modes" in type_defaults:
                config["supported_modes"] = type_defaults["supported_modes"]

        return config

    return set_defaults_


CONFIG_SCHEMA = cv.Schema(
    climate.climate_schema(TrumaClimate).extend({
        cv.GenerateID(): cv.declare_id(TrumaClimate),
        cv.GenerateID(CONF_TRUMA_INETBOX_ID): cv.use_id(TrumaINetBoxApp),
        cv.Required(CONF_TYPE): cv.enum(CONF_SUPPORTED_TYPE, upper=True),
        cv.Optional(CONF_NAME, default="Truma Climate"): cv.string,
        cv.Optional("preset"): cv.All(cv.ensure_list(cv.string)),  # If you want presets
        cv.Optional("supported_modes"): cv.ensure_list(cv.enum(CLIMATE_MODES, upper=True)),
    }).extend(cv.COMPONENT_SCHEMA).extend({
        cv.Optional("disabled_by_default", default=False): cv.boolean,
        cv.Optional("entity_category"): cv.entity_category,
        cv.Optional("icon"): cv.icon,
        cv.Optional(CONF_VISUAL, default={}): CLIMATE_VISUAL_SCHEMA,
    })
)

FINAL_VALIDATE_SCHEMA = set_default_based_on_type()


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETBOX_ID])

    # Get type-specific defaults
    type_key = config[CONF_TYPE].upper()
    type_defaults = CONF_TYPE_DEFAULTS.get(type_key, {})

    # Supported modes
    supported_modes = config.get("supported_modes", type_defaults.get("supported_modes", ["OFF", "HEAT"]))
    modes = [CLIMATE_MODES[m] for m in supported_modes]
    cg.add(var.set_supported_modes(modes))