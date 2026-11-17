"""Settings as number entities created by li_battery itself (kept in flash), e.g.

number:
  - platform: li_battery
    li_battery_id: bat
    type: capacity
    name: Battery Capacity
    initial_value: 2900
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_INITIAL_VALUE, CONF_RESTORE_VALUE, CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_LI_BATTERY_ID, LiBattery, li_battery_ns

DEPENDENCIES = ["li_battery"]

LiBatteryNumber = li_battery_ns.class_("LiBatteryNumber", number.Number, cg.Component)

# type: (setter on LiBattery, min, max, step, unit, icon)
TYPES = {
    "capacity": ("capacity", 0, 100000, 100, "mAh", "mdi:battery-plus-variant"),
    "internal_resistance": ("internal_resistance", 0, 2, 0.001, "Ω", "mdi:omega"),
    "cells_series": ("cells_series", 1, 24, 1, None, "mdi:battery-arrow-up"),
    "cells_parallel": ("cells_parallel", 1, 100, 1, None, "mdi:battery-plus"),
    "low": ("low", 0, 100, 1, "%", "mdi:battery-low"),
    "critical": ("critical", 2.0, 4.2, 0.01, "V", "mdi:battery-alert-variant-outline"),
    "resume": ("resume", 2.0, 4.2, 0.01, "V", "mdi:battery-arrow-up-outline"),
}


def _schema(config):
    t = TYPES[config[CONF_TYPE]]
    return number.number_schema(
        LiBatteryNumber,
        unit_of_measurement=t[4] or cv.UNDEFINED,
        icon=t[5],
        entity_category=ENTITY_CATEGORY_CONFIG,
    ).extend(
        {
            cv.GenerateID(CONF_LI_BATTERY_ID): cv.use_id(LiBattery),
            cv.Required(CONF_TYPE): cv.one_of(*TYPES, lower=True),
            cv.Optional(CONF_INITIAL_VALUE): cv.float_,
            cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA)(config)


def _pre(config):
    cv.Schema({cv.Required(CONF_TYPE): cv.one_of(*TYPES, lower=True)}, extra=cv.ALLOW_EXTRA)(config)
    return _schema(config)


CONFIG_SCHEMA = _pre


async def to_code(config):
    t = TYPES[config[CONF_TYPE]]
    var = await number.new_number(config, min_value=t[1], max_value=t[2], step=t[3])
    await cg.register_component(var, config)
    cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))
    if CONF_INITIAL_VALUE in config:
        cg.add(var.set_initial_value(config[CONF_INITIAL_VALUE]))
    hub = await cg.get_variable(config[CONF_LI_BATTERY_ID])
    cg.add(cg.RawStatement(f"{hub}->{t[0]}().num = {var};"))
