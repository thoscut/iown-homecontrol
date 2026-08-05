"""ESPHome component for the io-homecontrol protocol.

This component provides support for the io-homecontrol protocol used by
Somfy, Velux, and other smart home devices in the 868 MHz band.

It uses RadioLib for radio abstraction and supports SX1276/SX1262 radio modules
commonly found on LoRa32 boards (Heltec, LilyGo, etc.).

Note: This component is experimental. It supports frame reception with MAC and
replay checking, 1W authenticated cover commands, and 2W commands over a
challenge-response session (`two_way: true`).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@thoscut"]
DEPENDENCIES = []

# The hub compiles iown_cover.cpp and iown_sensor.cpp unconditionally - ESPHome
# builds every source in a component directory - and those include
# esphome/components/{cover,sensor}/*.h. Without this, a configuration that uses
# only covers, or only sensors, fails with "esphome/components/sensor/sensor.h:
# No such file or directory". The example config happens to use both, which is
# why it never showed up.
AUTO_LOAD = ["cover", "sensor"]
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
CONF_BUSY_PIN = "busy_pin"
CONF_FREQUENCY = "frequency"
CONF_RADIO_TYPE = "radio_type"
CONF_SOURCE_ADDRESS = "source_address"
CONF_SCK_PIN = "sck_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_MISO_PIN = "miso_pin"
CONF_SYSTEM_KEY = "system_key"
CONF_ENCRYPTION_ENABLED = "encryption_enabled"
CONF_ACEI = "acei"
CONF_ORIGINATOR = "originator"
CONF_POSITION_FEEDBACK = "position_feedback"
CONF_TWO_WAY = "two_way"

# The three io-homecontrol channels. 1W traffic only ever uses channel 2.
IOHC_CHANNELS = (868.25, 868.95, 869.85)

# docs/commands.md - "Command Originator"
ORIGINATORS = {
    "LOCAL_USER": 0x00,
    "USER": 0x01,
    "SENSOR_RAIN": 0x02,
    "SENSOR_TIMER": 0x03,
    "SECURITY": 0x04,
    "UPS": 0x05,
    "SFC": 0x06,
    "LSC": 0x07,
    "SAAC": 0x08,
    "SENSOR_WIND": 0x09,
    "LOAD_SHEDDING": 0x0B,
    "SENSOR_LIGHT": 0x0C,
    "SENSOR_ENVIRONMENT": 0x0D,
    "AUTOMATIC_CYCLE": 0xFE,
    "EMERGENCY": 0xFF,
}


def _validate_system_key(value):
    """Validate a 32-character hexadecimal AES-128 key."""
    value = cv.string_strict(value)
    stripped = value.replace(" ", "").replace(":", "").replace("-", "")
    if len(stripped) != 32:
        raise cv.Invalid(
            f"system_key must be 32 hexadecimal characters (16 bytes), got {len(stripped)}"
        )
    if not all(c in "0123456789abcdefABCDEF" for c in stripped):
        raise cv.Invalid("system_key must contain only hexadecimal characters (0-9, a-f, A-F)")
    if stripped.lower() == "0" * 32:
        raise cv.Invalid(
            "system_key must not be all zeroes - an all-zero key authenticates nothing"
        )
    return stripped


def _validate_acei(value):
    """Validate the ACEI byte.

    docs/commands.md: "LSB must be 1: The frame is not considered valid if this
    bit is set to 0!" An actuator silently discards a frame with an even ACEI,
    which is very hard to debug from the outside - so reject it here.
    """
    value = cv.hex_int_range(min=0x00, max=0xFF)(value)
    if value & 0x01 == 0:
        raise cv.Invalid(
            f"ACEI 0x{value:02X} has its 'IsValid' bit (bit 0) clear; actuators "
            f"discard such frames. Use 0x{value | 1:02X} instead."
        )
    return value


def _validate_frequency(value):
    """Warn about frequencies that are not one of the three io-homecontrol channels."""
    value = cv.float_range(min=863.0, max=870.0)(value)
    if not any(abs(value - channel) < 0.01 for channel in IOHC_CHANNELS):
        raise cv.Invalid(
            f"{value} MHz is not an io-homecontrol channel. "
            f"Use one of {', '.join(str(c) for c in IOHC_CHANNELS)}."
        )
    return value


def _validate_node_address(value):
    """Validate a 3-byte node address."""
    value = cv.hex_int_range(min=0x000000, max=0xFFFFFF)(value)
    if value == 0x000000:
        raise cv.Invalid(
            "source_address 0x000000 is the group address and must not be used "
            "as a controller's own address"
        )
    return value


def _validate_radio_pins(config):
    """Each radio family needs a different set of pins.

    RadioLib takes Module(cs, irq, rst, gpio), and the two families put
    different physical lines in those slots: an SX127x interrupts on DIO0 and
    uses DIO1 as its second GPIO, while an SX126x has no DIO0 at all - it
    interrupts on DIO1 and needs its BUSY line.

    The component used to pass `dio0_pin` as the interrupt for both, so an
    SX1262 only worked if you put its DIO1 in `dio0_pin` and its BUSY in
    `dio1_pin`. Filling the fields in with what the pins are actually called
    produced a radio whose interrupt never fired, with nothing to say why.
    """
    # cv.enum keeps the key from RADIO_TYPES and hangs the codegen expression
    # off it as .enum_value, so the value compares equal to the string the user
    # wrote. Comparing against the C++ enum name instead silently never matched.
    is_sx1262 = config[CONF_RADIO_TYPE] == "SX1262"

    if is_sx1262:
        if CONF_BUSY_PIN not in config:
            raise cv.Invalid(
                "radio_type SX1262 requires busy_pin. On a Heltec V3 the radio "
                "is CS=8, RST=12, BUSY=13, DIO1=14.",
                path=[CONF_RADIO_TYPE],
            )
        if CONF_DIO0_PIN in config:
            raise cv.Invalid(
                "SX126x modules have no DIO0. Earlier versions of this component "
                "wanted DIO1 in dio0_pin and BUSY in dio1_pin; give them as "
                "dio1_pin and busy_pin now.",
                path=[CONF_DIO0_PIN],
            )
    else:
        if CONF_DIO0_PIN not in config:
            raise cv.Invalid(
                "radio_type SX1276 requires dio0_pin - that is the line the "
                "radio raises when a packet has arrived.",
                path=[CONF_RADIO_TYPE],
            )
        if CONF_BUSY_PIN in config:
            raise cv.Invalid(
                "SX127x modules have no BUSY line.",
                path=[CONF_BUSY_PIN],
            )

    return config


def _validate_config(config):
    """Cross-field checks that a per-key validator cannot express."""
    _validate_radio_pins(config)

    if config[CONF_TWO_WAY] and CONF_SYSTEM_KEY not in config:
        raise cv.Invalid(
            "two_way requires system_key: the challenge-response handshake "
            "signs every frame with it, and without one no session can be "
            "established at all",
            path=[CONF_TWO_WAY],
        )

    if config[CONF_ENCRYPTION_ENABLED] and CONF_SYSTEM_KEY not in config:
        raise cv.Invalid(
            "encryption_enabled requires system_key: authenticated 1W frames "
            "cannot be built without the system key",
            path=[CONF_ENCRYPTION_ENABLED],
        )

    # SPI pins only take effect as a complete set.
    spi_pins = [CONF_SCK_PIN, CONF_MOSI_PIN, CONF_MISO_PIN]
    present = [pin for pin in spi_pins if pin in config]
    if present and len(present) != len(spi_pins):
        missing = ", ".join(pin for pin in spi_pins if pin not in config)
        raise cv.Invalid(
            f"custom SPI pins must all be given together; missing: {missing}",
            path=[present[0]],
        )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(IOWNHomeControlComponent),
            cv.Required(CONF_CS_PIN): cv.int_range(min=0, max=48),
            cv.Required(CONF_RST_PIN): cv.int_range(min=0, max=48),
            # Which of these is required depends on radio_type; see
            # _validate_radio_pins().
            cv.Optional(CONF_DIO0_PIN): cv.int_range(min=0, max=48),
            cv.Required(CONF_DIO1_PIN): cv.int_range(min=0, max=48),
            cv.Optional(CONF_BUSY_PIN): cv.int_range(min=0, max=48),
            cv.Optional(CONF_FREQUENCY, default=868.95): _validate_frequency,
            cv.Optional(CONF_RADIO_TYPE, default="SX1276"): cv.enum(
                RADIO_TYPES, upper=True
            ),
            cv.Optional(CONF_SOURCE_ADDRESS, default=0x1A380B): _validate_node_address,
            cv.Optional(CONF_SCK_PIN): cv.int_range(min=0, max=48),
            cv.Optional(CONF_MOSI_PIN): cv.int_range(min=0, max=48),
            cv.Optional(CONF_MISO_PIN): cv.int_range(min=0, max=48),
            cv.Optional(CONF_SYSTEM_KEY): _validate_system_key,
            cv.Optional(CONF_ENCRYPTION_ENABLED, default=False): cv.boolean,
            cv.Optional(CONF_ACEI, default=0x61): _validate_acei,
            cv.Optional(CONF_ORIGINATOR, default="USER"): cv.enum(
                ORIGINATORS, upper=True
            ),
            cv.Optional(CONF_POSITION_FEEDBACK, default=False): cv.boolean,
            # 2W: every frame is signed against a nonce the actuator chose
            # moments earlier, so a recorded frame stops working when the
            # session ends. 1W relies on a rolling code instead.
            cv.Optional(CONF_TWO_WAY, default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_config,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_cs_pin(config[CONF_CS_PIN]))
    cg.add(var.set_rst_pin(config[CONF_RST_PIN]))
    cg.add(var.set_dio1_pin(config[CONF_DIO1_PIN]))
    if CONF_DIO0_PIN in config:
        cg.add(var.set_dio0_pin(config[CONF_DIO0_PIN]))
    if CONF_BUSY_PIN in config:
        cg.add(var.set_busy_pin(config[CONF_BUSY_PIN]))
    cg.add(var.set_frequency(config[CONF_FREQUENCY]))
    cg.add(var.set_radio_type(config[CONF_RADIO_TYPE]))
    cg.add(var.set_source_address(config[CONF_SOURCE_ADDRESS]))
    cg.add(var.set_acei(config[CONF_ACEI]))
    cg.add(var.set_originator(config[CONF_ORIGINATOR]))
    cg.add(var.set_position_feedback(config[CONF_POSITION_FEEDBACK]))
    cg.add(var.set_two_way(config[CONF_TWO_WAY]))

    if CONF_SCK_PIN in config:
        cg.add(var.set_sck_pin(config[CONF_SCK_PIN]))
        cg.add(var.set_mosi_pin(config[CONF_MOSI_PIN]))
        cg.add(var.set_miso_pin(config[CONF_MISO_PIN]))

    if CONF_SYSTEM_KEY in config:
        key_hex = config[CONF_SYSTEM_KEY]
        key_bytes = [int(key_hex[i : i + 2], 16) for i in range(0, 32, 2)]
        cg.add(var.set_system_key(key_bytes))

    cg.add(var.set_encryption_enabled(config[CONF_ENCRYPTION_ENABLED]))

    # RadioLib's Module.h includes <SPI.h>. ESPHome does not put the Arduino
    # SPI library on the include path unless it is declared, so without this
    # the component fails to compile with "SPI.h: No such file or directory".
    cg.add_library("SPI", None)

    # Pin the radio library: an unpinned dependency makes the build depend on
    # whatever upstream published most recently.
    #
    # This must match the pin in platformio.ini. It said 7.1.2 while the
    # firmware built against 7.7.1, so the two halves of this repository were
    # compiled against different versions of the same library and only one of
    # them was covered by tools/check_radiolib_mock.sh. CI now compares them.
    cg.add_library("jgromes/RadioLib", "7.7.1")
