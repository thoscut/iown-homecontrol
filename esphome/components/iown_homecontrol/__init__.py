"""ESPHome component for io-homecontrol protocol.

This component provides support for the io-homecontrol protocol used by
Somfy, Velux, and other smart home devices in the 868 MHz band.

It uses RadioLib for radio abstraction and supports SX1276/SX1262 radio modules
commonly found on LoRa32 boards (Heltec, LilyGo, etc.).

Note: This component is experimental. The io-homecontrol protocol implementation
is still work in progress. Basic frame reception and 2W cover commands are supported.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@thoscut"]
DEPENDENCIES = []
AUTO_LOAD = []
MULTI_CONF = False

iown_homecontrol_ns = cg.esphome_ns.namespace("iown_homecontrol")
IOWNHomeControlComponent = iown_homecontrol_ns.class_(
    "IOWNHomeControlComponent", cg.Component
)

RadioType = iown_homecontrol_ns.enum("RadioType")
RADIO_TYPES = {
    "SX1276": RadioType.RADIO_SX1276,
    "SX1262": RadioType.RADIO_SX1262,
}

CONF_CS_PIN = "cs_pin"
CONF_RST_PIN = "rst_pin"
CONF_DIO0_PIN = "dio0_pin"
CONF_DIO1_PIN = "dio1_pin"
CONF_FREQUENCY = "frequency"
CONF_RADIO_TYPE = "radio_type"
CONF_SOURCE_ADDRESS = "source_address"
CONF_SCK_PIN = "sck_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_MISO_PIN = "miso_pin"
CONF_SYSTEM_KEY = "system_key"
CONF_ENCRYPTION_ENABLED = "encryption_enabled"

def _validate_hex_string(value):
    """Validate that a string contains only hexadecimal characters."""
    if not all(c in "0123456789abcdefABCDEF" for c in value):
        raise cv.Invalid("must contain only hexadecimal characters (0-9, a-f, A-F)")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(IOWNHomeControlComponent),
        cv.Required(CONF_CS_PIN): cv.int_range(min=0, max=48),
        cv.Required(CONF_RST_PIN): cv.int_range(min=0, max=48),
        cv.Required(CONF_DIO0_PIN): cv.int_range(min=0, max=48),
        cv.Required(CONF_DIO1_PIN): cv.int_range(min=0, max=48),
        cv.Optional(CONF_FREQUENCY, default=868.95): cv.float_range(
            min=868.0, max=870.0
        ),
        cv.Optional(CONF_RADIO_TYPE, default="SX1276"): cv.enum(
            RADIO_TYPES, upper=True
        ),
        cv.Optional(CONF_SOURCE_ADDRESS, default=0x1A380B): cv.int_range(
            min=0, max=0xFFFFFF
        ),
        cv.Optional(CONF_SCK_PIN): cv.int_range(min=0, max=48),
        cv.Optional(CONF_MOSI_PIN): cv.int_range(min=0, max=48),
        cv.Optional(CONF_MISO_PIN): cv.int_range(min=0, max=48),
        cv.Optional(CONF_SYSTEM_KEY): cv.All(
            cv.string, cv.Length(min=32, max=32), _validate_hex_string
        ),
        cv.Optional(CONF_ENCRYPTION_ENABLED, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_cs_pin(config[CONF_CS_PIN]))
    cg.add(var.set_rst_pin(config[CONF_RST_PIN]))
    cg.add(var.set_dio0_pin(config[CONF_DIO0_PIN]))
    cg.add(var.set_dio1_pin(config[CONF_DIO1_PIN]))
    cg.add(var.set_frequency(config[CONF_FREQUENCY]))
    cg.add(var.set_radio_type(config[CONF_RADIO_TYPE]))
    cg.add(var.set_source_address(config[CONF_SOURCE_ADDRESS]))

    if CONF_SCK_PIN in config:
        cg.add(var.set_sck_pin(config[CONF_SCK_PIN]))
    if CONF_MOSI_PIN in config:
        cg.add(var.set_mosi_pin(config[CONF_MOSI_PIN]))
    if CONF_MISO_PIN in config:
        cg.add(var.set_miso_pin(config[CONF_MISO_PIN]))

    if CONF_SYSTEM_KEY in config:
        key_hex = config[CONF_SYSTEM_KEY]
        key_bytes = [int(key_hex[i : i + 2], 16) for i in range(0, 32, 2)]
        cg.add(var.set_system_key(key_bytes))
    if config.get(CONF_ENCRYPTION_ENABLED):
        cg.add(var.set_encryption_enabled(True))

    # Add RadioLib as a library dependency
    cg.add_library("jgromes/RadioLib", "7.1.2")
