import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID
from . import gecko_spa_ns, GeckoSpa

DEPENDENCIES = ["gecko_spa"]

CONF_GECKO_SPA_ID = "gecko_spa_id"
CONF_SENSOR_TYPE = "type"

SENSOR_TYPES = {
    "standby": "STANDBY",
    "connected": "CONNECTED",
    "waterfall": "WATERFALL",
    "blower": "BLOWER",
    "quiet_time": "QUIET_TIME",  # On while quiet time is holding the pumps off
    "economy": "ECONOMY",  # Economy (lower setpoint) in effect
}

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema().extend(
    {
        cv.GenerateID(CONF_GECKO_SPA_ID): cv.use_id(GeckoSpa),
        cv.Required(CONF_SENSOR_TYPE): cv.enum(SENSOR_TYPES, lower=True),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_GECKO_SPA_ID])
    var = await binary_sensor.new_binary_sensor(config)

    sensor_type = config[CONF_SENSOR_TYPE]
    if sensor_type == "standby":
        cg.add(parent.set_standby_sensor(var))
    elif sensor_type == "connected":
        cg.add(parent.set_connected_sensor(var))
    elif sensor_type == "waterfall":
        cg.add(parent.set_waterfall_sensor(var))
    elif sensor_type == "blower":
        cg.add(parent.set_blower_sensor(var))
    elif sensor_type == "quiet_time":
        cg.add(parent.set_quiet_time_sensor(var))
    elif sensor_type == "economy":
        cg.add(parent.set_economy_sensor(var))
