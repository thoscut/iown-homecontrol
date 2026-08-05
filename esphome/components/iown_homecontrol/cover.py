"""ESPHome cover platform for io-homecontrol blinds, shutters and windows.

Each cover is identified by the 3-byte node address of the actuator it drives.
0x00003F is the io-homecontrol broadcast address, which every actuator in
range will act on - handy for a first test, unsuitable for normal use.

Position is estimated from the configured travel durations, so it is reported
as an assumed state unless you turn `assumed_state` off.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import cover
from esphome.const import CONF_ID, CONF_RESTORE_MODE

from . import iown_homecontrol_ns, IOWNHomeControlComponent

DEPENDENCIES = ["iown_homecontrol"]

IOWNCover = iown_homecontrol_ns.class_("IOWNCover", cover.Cover, cg.Component)
CoverRestoreMode = iown_homecontrol_ns.enum("CoverRestoreMode")

CONF_IOWN_ID = "iown_homecontrol_id"
CONF_TARGET_ADDRESS = "target_address"
CONF_FULL_OPEN_DURATION = "full_open_duration"
CONF_FULL_CLOSE_DURATION = "full_close_duration"
CONF_SUPPORTS_TILT = "supports_tilt"
CONF_ASSUMED_STATE = "assumed_state"
CONF_INITIAL_POSITION = "initial_position"

RESTORE_MODES = {
    "NO_RESTORE": CoverRestoreMode.COVER_RESTORE_NONE,
    "RESTORE": CoverRestoreMode.COVER_RESTORE_FROM_NVS,
}

# The broadcast address every actuator responds to.
BROADCAST_ADDRESS = 0x00003F


def _validate_target_address(value):
    return cv.hex_int_range(min=0x000000, max=0xFFFFFF)(value)


def _validate_durations(config):
    """A zero travel duration would make the position estimate meaningless."""
    for key in (CONF_FULL_OPEN_DURATION, CONF_FULL_CLOSE_DURATION):
        if config[key].total_milliseconds <= 0:
            raise cv.Invalid(f"{key} must be greater than zero", path=[key])
    return config


CONFIG_SCHEMA = cv.All(
    cover.cover_schema(IOWNCover)
    .extend(
        {
            cv.GenerateID(CONF_IOWN_ID): cv.use_id(IOWNHomeControlComponent),
            cv.Optional(
                CONF_TARGET_ADDRESS, default=BROADCAST_ADDRESS
            ): _validate_target_address,
            cv.Optional(
                CONF_FULL_OPEN_DURATION, default="30s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_FULL_CLOSE_DURATION, default="30s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SUPPORTS_TILT, default=False): cv.boolean,
            cv.Optional(CONF_ASSUMED_STATE, default=True): cv.boolean,
            cv.Optional(CONF_RESTORE_MODE, default="NO_RESTORE"): cv.enum(
                RESTORE_MODES, upper=True, space="_"
            ),
            cv.Optional(CONF_INITIAL_POSITION, default=1.0): cv.percentage,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_durations,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cover.register_cover(var, config)

    hub = await cg.get_variable(config[CONF_IOWN_ID])
    cg.add(var.set_hub(hub))
    cg.add(var.set_target_address(config[CONF_TARGET_ADDRESS]))
    cg.add(var.set_open_duration(config[CONF_FULL_OPEN_DURATION]))
    cg.add(var.set_close_duration(config[CONF_FULL_CLOSE_DURATION]))
    cg.add(var.set_supports_tilt(config[CONF_SUPPORTS_TILT]))
    cg.add(var.set_assumed_state(config[CONF_ASSUMED_STATE]))
    cg.add(var.set_restore_mode(config[CONF_RESTORE_MODE]))
    cg.add(var.set_initial_position(config[CONF_INITIAL_POSITION]))
