"""ESPHome cover platform for io-homecontrol blinds/shutters.

Supports basic OPEN, CLOSE, and STOP commands for io-homecontrol covers.
Each cover is identified by a target address (3-byte node ID).
Use 0x00003F for broadcast to all devices.

Note: This uses 2W (two-way) frame format without encryption.
Actual device pairing and encrypted 1W commands are not yet supported.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import cover
from esphome.const import CONF_ID

from . import iown_homecontrol_ns, IOWNHomeControlComponent

DEPENDENCIES = ["iown_homecontrol"]

IOWNCover = iown_homecontrol_ns.class_("IOWNCover", cover.Cover, cg.Component)

CONF_IOWN_ID = "iown_homecontrol_id"
CONF_TARGET_ADDRESS = "target_address"

CONFIG_SCHEMA = (
    cover.COVER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(IOWNCover),
            cv.GenerateID(CONF_IOWN_ID): cv.use_id(IOWNHomeControlComponent),
            cv.Optional(CONF_TARGET_ADDRESS, default=0x00003F): cv.int_range(
                min=0, max=0xFFFFFF
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cover.register_cover(var, config)

    hub = await cg.get_variable(config[CONF_IOWN_ID])
    cg.add(var.set_hub(hub))
    cg.add(var.set_target_address(config[CONF_TARGET_ADDRESS]))
