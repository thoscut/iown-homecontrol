"""Diagnostic sensors for the io-homecontrol hub.

Exposes the link quality and receive-path counters so a flaky installation can
be diagnosed from Home Assistant instead of the serial log:

  - ``rssi``            signal strength of the last accepted frame
  - ``frames_received`` frames that passed the CRC check
  - ``crc_errors``      frames dropped because the CRC did not match
  - ``rolling_code``    current 1W sequence number
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_EMPTY,
)

from . import iown_homecontrol_ns, IOWNHomeControlComponent

DEPENDENCIES = ["iown_homecontrol"]

IOWNSensor = iown_homecontrol_ns.class_(
    "IOWNSensor", cg.PollingComponent
)

CONF_IOWN_ID = "iown_homecontrol_id"
CONF_RSSI = "rssi"
CONF_FRAMES_RECEIVED = "frames_received"
CONF_CRC_ERRORS = "crc_errors"
CONF_ROLLING_CODE = "rolling_code"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(IOWNSensor),
        cv.GenerateID(CONF_IOWN_ID): cv.use_id(IOWNHomeControlComponent),
        cv.Optional(CONF_RSSI): sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_FRAMES_RECEIVED): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_CRC_ERRORS): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_ROLLING_CODE): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
).extend(cv.polling_component_schema("60s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    hub = await cg.get_variable(config[CONF_IOWN_ID])
    cg.add(var.set_hub(hub))

    for key, setter in (
        (CONF_RSSI, var.set_rssi_sensor),
        (CONF_FRAMES_RECEIVED, var.set_frames_received_sensor),
        (CONF_CRC_ERRORS, var.set_crc_errors_sensor),
        (CONF_ROLLING_CODE, var.set_rolling_code_sensor),
    ):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(setter(sens))
