import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import time as time_
from esphome.components import uart
from esphome.const import (
    CONF_ADDRESS,
    CONF_FREQUENCY,
    CONF_HOUR,
    CONF_ID,
    CONF_MAX_TEMPERATURE,
    CONF_MIN_TEMPERATURE,
    CONF_MINUTE,
    CONF_SCL,
    CONF_SDA,
    CONF_TIME_ID,
)
from esphome.core import CORE

# Chips with a single I2C peripheral, where only bus 0 exists.
SINGLE_I2C_VARIANTS = {"ESP32C3"}

# ESP-IDF's version 2 I2C slave driver, the one that reports where each
# transaction ends, is only available from this release on.
MIN_IDF_VERSION = cv.Version(5, 4, 0)
# ESP-IDF 6.0 dropped version 1 of the slave driver, leaving version 2 - the
# same API - as the only one, so the option that selects it is gone too.
IDF_SLAVE_V2_ONLY_VERSION = cv.Version(6, 0, 0)

# The highest max_temperature allowed: 106 F, the maximum an inYT pack reports
# (740 in its 1/18 C units). Written this way so "106°F" is accepted too.
MAX_SETPOINT = 740 / 18

AUTO_LOAD = ["climate", "switch", "select", "binary_sensor", "text_sensor"]

CONF_UART_ID = "uart_id"
CONF_RESET_PIN = "reset_pin"
CONF_I2C_BUS = "i2c_bus"
CONF_NOTIF_DATE_FORMAT = "notif_date_format"
CONF_UART_TRANSPORT_ID = "uart_transport_id"
CONF_QUIET_TIME = "quiet_time"
CONF_START = "start"
CONF_END = "end"
CONF_MIN_WATER_TEMPERATURE = "min_water_temperature"
CONF_I2C_TRANSPORT_ID = "i2c_transport_id"

gecko_spa_ns = cg.esphome_ns.namespace("gecko_spa")
GeckoSpa = gecko_spa_ns.class_("GeckoSpa", cg.Component)
GeckoTransport = gecko_spa_ns.class_("GeckoTransport")
GeckoUartTransport = gecko_spa_ns.class_(
    "GeckoUartTransport", GeckoTransport, uart.UARTDevice
)
GeckoI2cTransport = gecko_spa_ns.class_("GeckoI2cTransport", GeckoTransport)

NotifDateFormat = gecko_spa_ns.enum("NotifDateFormat", is_class=True)
NOTIF_DATE_FORMATS = {
    "Y-M-D": NotifDateFormat.Y_M_D,
    "D-M-Y": NotifDateFormat.D_M_Y,
}

I2C_KEYS = (CONF_SDA, CONF_SCL)


def _minute_of_day(value):
    return value[CONF_HOUR] * 60 + value[CONF_MINUTE]


def _validate_quiet_time(value):
    if _minute_of_day(value[CONF_START]) == _minute_of_day(value[CONF_END]):
        raise cv.Invalid("'start' and 'end' must be different times")
    return value


# Every night between start and end, hold the spa in standby (the panel's
# Maintenance mode: all pumps off). Standby also stops the heater, so quiet
# time gives up for the night if the water drops below min_water_temperature.
QUIET_TIME_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Required(CONF_START): cv.time_of_day,
            cv.Required(CONF_END): cv.time_of_day,
            cv.Optional(CONF_MIN_WATER_TEMPERATURE, default="10°C"): cv.temperature,
        }
    ),
    _validate_quiet_time,
)


def _validate_transport(config):
    """Pick between the Arduino proxy and driving the spa bus directly."""
    uses_uart = CONF_UART_ID in config
    uses_i2c = any(key in config for key in I2C_KEYS)

    if uses_uart and uses_i2c:
        raise cv.Invalid(
            f"Use either '{CONF_UART_ID}' (Arduino proxy) or "
            f"'{CONF_SDA}'/'{CONF_SCL}' (direct I2C), not both"
        )
    if not uses_uart and not uses_i2c:
        raise cv.Invalid(
            f"Set '{CONF_SDA}' and '{CONF_SCL}' to drive the spa bus directly, or "
            f"'{CONF_UART_ID}' to talk through an Arduino I2C proxy"
        )

    if uses_uart:
        return config

    for key in I2C_KEYS:
        if key not in config:
            raise cv.Invalid(f"'{key}' is required when using direct I2C", path=[key])
    if CONF_RESET_PIN in config:
        raise cv.Invalid(
            f"'{CONF_RESET_PIN}' resets the Arduino proxy and has no meaning in "
            "direct I2C mode",
            path=[CONF_RESET_PIN],
        )
    if not CORE.is_esp32:
        raise cv.Invalid("Direct I2C mode requires an ESP32")
    if not CORE.using_arduino:
        from esphome.components.esp32 import idf_version

        if idf_version() < MIN_IDF_VERSION:
            raise cv.Invalid(
                f"Direct I2C mode on ESP-IDF needs ESP-IDF {MIN_IDF_VERSION} or newer "
                f"(this build uses {idf_version()}). Update ESPHome, or use "
                "'framework: type: arduino'"
            )
    if config[CONF_SDA] == config[CONF_SCL]:
        raise cv.Invalid(f"'{CONF_SDA}' and '{CONF_SCL}' must be different pins")

    from esphome.components.esp32 import get_esp32_variant

    if config[CONF_I2C_BUS] != 0 and get_esp32_variant() in SINGLE_I2C_VARIANTS:
        raise cv.Invalid(
            f"{get_esp32_variant()} has a single I2C peripheral; '{CONF_I2C_BUS}' must be 0",
            path=[CONF_I2C_BUS],
        )
    return config


def _validate_setpoint_range(config):
    if config[CONF_MIN_TEMPERATURE] >= config[CONF_MAX_TEMPERATURE]:
        raise cv.Invalid(
            f"'{CONF_MIN_TEMPERATURE}' must be below '{CONF_MAX_TEMPERATURE}'",
            path=[CONF_MIN_TEMPERATURE],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GeckoSpa),
            cv.GenerateID(CONF_UART_TRANSPORT_ID): cv.declare_id(GeckoUartTransport),
            cv.GenerateID(CONF_I2C_TRANSPORT_ID): cv.declare_id(GeckoI2cTransport),
            # Arduino I2C proxy over UART (legacy wiring)
            cv.Optional(CONF_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            # Direct connection to the spa I2C bus (no Arduino)
            cv.Optional(CONF_SDA): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_SCL): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_I2C_BUS, default=0): cv.int_range(min=0, max=1),
            cv.Optional(CONF_ADDRESS, default=0x17): cv.i2c_address,
            cv.Optional(CONF_FREQUENCY, default="100kHz"): cv.All(
                cv.frequency, cv.Range(min=10000, max=400000)
            ),
            cv.Optional(CONF_NOTIF_DATE_FORMAT, default="D-M-Y"): cv.enum(
                NOTIF_DATE_FORMATS, upper=True
            ),
            # The setpoint range Home Assistant offers. Each pack reports its
            # own (MinSetpointG/MaxSetpointG, shown in the log); match them.
            cv.Optional(CONF_MIN_TEMPERATURE, default="26°C"): cv.All(
                cv.temperature, cv.Range(min=10.0, max=40.0)
            ),
            # Gecko panels stop at 40 C unless the up key is held, so going
            # higher is opt-in.
            cv.Optional(CONF_MAX_TEMPERATURE, default="40°C"): cv.All(
                cv.temperature,
                cv.Range(
                    min=30.0,
                    max=MAX_SETPOINT,
                    msg="max_temperature must be between 30°C and 41.1°C (106°F)",
                ),
            ),
            cv.Optional(CONF_QUIET_TIME): QUIET_TIME_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_transport,
    _validate_setpoint_range,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_UART_ID in config:
        cg.add_define("USE_GECKO_SPA_UART")
        transport = cg.new_Pvariable(config[CONF_UART_TRANSPORT_ID])
        uart_component = await cg.get_variable(config[CONF_UART_ID])
        cg.add(transport.set_uart_parent(uart_component))
        if CONF_RESET_PIN in config:
            pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
            cg.add(transport.set_reset_pin(pin))
    else:
        cg.add_define("USE_GECKO_SPA_I2C")
        if CORE.using_arduino:
            cg.add_define("USE_GECKO_SPA_I2C_ARDUINO_HAL")
        else:
            from esphome.components.esp32 import (
                add_idf_sdkconfig_option,
                idf_version,
                include_builtin_idf_component,
            )

            cg.add_define("USE_GECKO_SPA_I2C_IDF")
            # ESPHome leaves the IDF I2C driver out of the build unless a
            # component asks for it. Without it there are no driver headers,
            # and the option below is silently dropped along with its Kconfig.
            include_builtin_idf_component("esp_driver_i2c")
            # Version 1 of the IDF slave driver cannot tell where one
            # transaction ends and the next begins; version 2 can. From
            # ESP-IDF 6 it is the only one, with no option to set.
            if idf_version() < IDF_SLAVE_V2_ONLY_VERSION:
                add_idf_sdkconfig_option("CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2", True)
        transport = cg.new_Pvariable(config[CONF_I2C_TRANSPORT_ID])
        cg.add(transport.set_sda_pin(config[CONF_SDA]))
        cg.add(transport.set_scl_pin(config[CONF_SCL]))
        cg.add(transport.set_bus_num(config[CONF_I2C_BUS]))
        cg.add(transport.set_address(config[CONF_ADDRESS]))
        cg.add(transport.set_frequency(int(config[CONF_FREQUENCY])))

    cg.add(var.set_transport(transport))
    cg.add(var.set_notif_date_format(config[CONF_NOTIF_DATE_FORMAT]))
    cg.add(var.set_min_temperature(config[CONF_MIN_TEMPERATURE]))
    cg.add(var.set_max_temperature(config[CONF_MAX_TEMPERATURE]))

    if quiet := config.get(CONF_QUIET_TIME):
        cg.add_define("USE_GECKO_SPA_QUIET_TIME")
        clock = await cg.get_variable(quiet[CONF_TIME_ID])
        cg.add(
            var.set_quiet_time(
                clock,
                _minute_of_day(quiet[CONF_START]),
                _minute_of_day(quiet[CONF_END]),
                quiet[CONF_MIN_WATER_TEMPERATURE],
            )
        )
