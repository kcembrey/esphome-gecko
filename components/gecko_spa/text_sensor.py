import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID
from . import gecko_spa_ns, GeckoSpa

DEPENDENCIES = ["gecko_spa"]

CONF_GECKO_SPA_ID = "gecko_spa_id"
CONF_SENSOR_TYPE = "type"

SENSOR_TYPES = {
    "spa_time": "SPA_TIME",
    "rinse_filter": "RINSE_FILTER",
    "clean_filter": "CLEAN_FILTER",
    "change_water": "CHANGE_WATER",
    "spa_checkup": "SPA_CHECKUP",
    "config_version": "CONFIG_VERSION",
    "status_version": "STATUS_VERSION",
    "lock_mode": "LOCK_MODE",
    "pack_type": "PACK_TYPE",
    "filtration": "FILTRATION",  # What the active program has filtration doing
}

CONFIG_SCHEMA = text_sensor.text_sensor_schema().extend(
    {
        cv.GenerateID(CONF_GECKO_SPA_ID): cv.use_id(GeckoSpa),
        cv.Required(CONF_SENSOR_TYPE): cv.enum(SENSOR_TYPES, lower=True),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_GECKO_SPA_ID])
    var = await text_sensor.new_text_sensor(config)

    sensor_type = config[CONF_SENSOR_TYPE]
    if sensor_type == "spa_time":
        cg.add(parent.set_spa_time_sensor(var))
    elif sensor_type == "rinse_filter":
        cg.add(parent.set_rinse_filter_sensor(var))
    elif sensor_type == "clean_filter":
        cg.add(parent.set_clean_filter_sensor(var))
    elif sensor_type == "change_water":
        cg.add(parent.set_change_water_sensor(var))
    elif sensor_type == "spa_checkup":
        cg.add(parent.set_spa_checkup_sensor(var))
    elif sensor_type == "config_version":
        cg.add(parent.set_config_version_sensor(var))
    elif sensor_type == "status_version":
        cg.add(parent.set_status_version_sensor(var))
    elif sensor_type == "lock_mode":
        cg.add(parent.set_lock_mode_sensor(var))
    elif sensor_type == "pack_type":
        cg.add(parent.set_pack_type_sensor(var))
    elif sensor_type == "filtration":
        cg.add(parent.set_filtration_sensor(var))
