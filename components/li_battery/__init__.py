"""li_battery — state of charge for LiPo / Li-ion / LiFePO4 batteries.

Voltage only (ADC divider) or voltage + current / power (INA226 and the like).
https://github.com/daimonkor/esphome-li-battery
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, number, sensor, text_sensor
from esphome.const import (
    CONF_CURRENT,
    CONF_ID,
    CONF_POWER,
    CONF_UPDATE_INTERVAL,
    CONF_VOLTAGE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_HOUR,
    UNIT_PERCENT,
    UNIT_VOLT,
    UNIT_WATT,
)

CODEOWNERS = ["@daimonkor"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]
MULTI_CONF = True

CONF_LI_BATTERY_ID = "li_battery_id"
CONF_CHEMISTRY = "chemistry"
CONF_CURVE = "curve"
CONF_CELLS_SERIES = "cells_series"
CONF_CELLS_PARALLEL = "cells_parallel"
CONF_CAPACITY = "capacity"
CONF_INTERNAL_RESISTANCE = "internal_resistance"
CONF_RESTORE = "restore"
CONF_LOW = "low"
CONF_CRITICAL = "critical"
CONF_RESUME = "resume"
CONF_SMOOTHING = "smoothing"
CONF_ONLY_FALLS = "only_falls"
CONF_JUMP_UP = "jump_up"
CONF_CHARGE_ENTER_CURRENT = "charge_enter_current"
CONF_CHARGE_EXIT_CURRENT = "charge_exit_current"
CONF_DISCHARGE_CURRENT = "discharge_current"
CONF_DEBOUNCE = "debounce"
CONF_FULL_VOLTAGE = "full_voltage"
CONF_FULL_CURRENT = "full_current"
CONF_HOLD_AFTER_CHARGE = "hold_after_charge"
CONF_REPORT_STEP = "report_step"
CONF_REPORT_AVERAGE = "report_average"
CONF_CHARGE_DETECT = "charge_detect"
CONF_CHARGE_VOLTAGE = "charge_voltage"
CONF_CHARGE_JUMP = "charge_jump"
# outputs
CONF_PERCENT = "percent"
CONF_REST_VOLTAGE = "rest_voltage"
CONF_REPORT_VOLTAGE = "report_voltage"
CONF_REMAINING = "remaining"
CONF_TIME_LEFT = "time_left"
CONF_CHARGING = "charging"
CONF_STATUS = "status"
CONF_LOW_ALARM = "low_alarm"
CONF_CRITICAL_ALARM = "critical_alarm"

li_battery_ns = cg.esphome_ns.namespace("li_battery")
LiBattery = li_battery_ns.class_("LiBattery", cg.PollingComponent)
Chemistry = cg.global_ns.namespace("li_battery_core").enum("Chemistry", is_class=True)
Restore = li_battery_ns.enum("Restore", is_class=True)

CHEMISTRY = {
    "lipo": Chemistry.LIPO,
    "liion": Chemistry.LIION,
    "lifepo4": Chemistry.LIFEPO4,
    "custom": Chemistry.CUSTOM,
}
# per-cell defaults by chemistry: critical / resume / full voltage
CHEM_DEFAULTS = {
    "lipo": (3.50, 3.65, 4.15),
    "liion": (3.20, 3.40, 4.15),
    "lifepo4": (2.90, 3.10, 3.45),
    "custom": (None, None, 4.15),
}
# per-cell voltage at or above which a voltage-only battery is taken as on the charger (charge_detect)
CHARGE_V_DEFAULTS = {"lipo": 4.165, "liion": 4.165, "lifepo4": 3.50, "custom": 4.165}
RESTORE = {"none": Restore.NONE, "rtc": Restore.RTC}


def _param(validator):
    """A fixed value or the id of a number entity (changeable from HA)."""
    return cv.Any(validator, cv.use_id(number.Number))


def _curve_point(value):
    value = cv.Schema(
        {cv.Required(CONF_VOLTAGE): cv.voltage, cv.Required(CONF_PERCENT): cv.All(cv.percentage_int, cv.float_range(min=0, max=100))}
    )(value)
    return value


def _validate(config):
    chem = config[CONF_CHEMISTRY]
    if chem == "custom":
        if len(config.get(CONF_CURVE, [])) < 2:
            raise cv.Invalid("chemistry: custom needs a 'curve' with at least 2 points")
        volts = [p[CONF_VOLTAGE] for p in config[CONF_CURVE]]
        if volts != sorted(volts) or len(set(volts)) != len(volts):
            raise cv.Invalid("curve: voltages must be strictly increasing")
    elif CONF_CURVE in config:
        raise cv.Invalid("'curve' is only used with chemistry: custom")
    if config.get(CONF_CHARGE_DETECT) and (CONF_CURRENT in config or CONF_POWER in config):
        raise cv.Invalid("'charge_detect' is for voltage only; with a current the charging state comes from its sign")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LiBattery),
            # inputs (external sensors)
            cv.Optional(CONF_VOLTAGE): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_CURRENT): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_POWER): cv.use_id(sensor.Sensor),
            # battery
            cv.Optional(CONF_CHEMISTRY, default="lipo"): cv.one_of(*CHEMISTRY, lower=True),
            cv.Optional(CONF_CURVE): cv.ensure_list(_curve_point),
            cv.Optional(CONF_CELLS_SERIES, default=1): _param(cv.int_range(min=1, max=24)),
            cv.Optional(CONF_CELLS_PARALLEL, default=1): _param(cv.int_range(min=1, max=100)),
            cv.Optional(CONF_CAPACITY): _param(cv.float_range(min=0, max=1000000)),  # mAh per cell
            cv.Optional(CONF_INTERNAL_RESISTANCE): _param(cv.float_range(min=0, max=10)),  # ohm, pack
            cv.Optional(CONF_RESTORE, default="none"): cv.one_of(*RESTORE, lower=True),
            # levels
            cv.Optional(CONF_LOW, default=10): _param(cv.float_range(min=0, max=100)),  # %
            cv.Optional(CONF_CRITICAL): _param(cv.float_range(min=0, max=5)),  # V per cell
            cv.Optional(CONF_RESUME): _param(cv.float_range(min=0, max=5)),  # V per cell
            # voltage-only tuning
            cv.Optional(CONF_SMOOTHING): cv.float_range(min=0.01, max=1),
            cv.Optional(CONF_ONLY_FALLS): cv.boolean,
            cv.Optional(CONF_JUMP_UP, default=0.10): cv.float_range(min=0, max=2),
            cv.Optional(CONF_REPORT_STEP, default=0.03): cv.float_range(min=0, max=1),
            cv.Optional(CONF_REPORT_AVERAGE, default=10): cv.int_range(min=1, max=16),
            # voltage only: charging guessed from the voltage (jump / near-full, until it falls)
            cv.Optional(CONF_CHARGE_DETECT, default=False): cv.boolean,
            cv.Optional(CONF_CHARGE_VOLTAGE): cv.float_range(min=2, max=5),  # V per cell
            cv.Optional(CONF_CHARGE_JUMP, default=0.04): cv.float_range(min=0.01, max=1),  # V per cell
            # voltage + current tuning (A; minus = charge)
            cv.Optional(CONF_CHARGE_ENTER_CURRENT, default=-0.008): cv.float_,
            cv.Optional(CONF_CHARGE_EXIT_CURRENT, default=-0.004): cv.float_,
            cv.Optional(CONF_DISCHARGE_CURRENT, default=0.008): cv.float_,
            cv.Optional(CONF_DEBOUNCE, default=2): cv.int_range(min=1, max=50),
            cv.Optional(CONF_FULL_VOLTAGE): cv.float_range(min=2, max=5),  # V per cell
            cv.Optional(CONF_FULL_CURRENT, default=-0.05): cv.float_,
            cv.Optional(CONF_HOLD_AFTER_CHARGE, default="20min"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_UPDATE_INTERVAL, default="2s"): cv.update_interval,
            # outputs
            cv.Optional(CONF_PERCENT): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_REST_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                accuracy_decimals=3,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_REPORT_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_REMAINING): sensor.sensor_schema(
                unit_of_measurement="mAh",
                accuracy_decimals=0,
                icon="mdi:battery-heart-variant",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TIME_LEFT): sensor.sensor_schema(
                unit_of_measurement=UNIT_HOUR,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional("power_out"): sensor.sensor_schema(
                unit_of_measurement=UNIT_WATT,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_POWER,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_CHARGING): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_BATTERY_CHARGING
            ),
            cv.Optional(CONF_LOW_ALARM): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM, icon="mdi:battery-low"
            ),
            cv.Optional(CONF_CRITICAL_ALARM): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM, icon="mdi:battery-alert-variant-outline"
            ),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(icon="mdi:battery-heart-variant"),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    from esphome.core import ID

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    has_current = CONF_CURRENT in config or CONF_POWER in config
    if CONF_VOLTAGE in config:
        cg.add(var.set_voltage_sensor(await cg.get_variable(config[CONF_VOLTAGE])))
    if CONF_CURRENT in config:
        cg.add(var.set_current_sensor(await cg.get_variable(config[CONF_CURRENT])))
    if CONF_POWER in config:
        cg.add(var.set_power_sensor(await cg.get_variable(config[CONF_POWER])))
    if not has_current:
        # voltage only: driven by the voltage sensor (or process_voltage()), no polling
        cg.add(var.set_update_interval(4294967295))

    chem = config[CONF_CHEMISTRY]
    cg.add(var.set_chemistry(CHEMISTRY[chem]))
    for p in config.get(CONF_CURVE, []):
        cg.add(var.add_curve_point(p[CONF_VOLTAGE], float(p[CONF_PERCENT])))
    cg.add(var.set_restore(RESTORE[config[CONF_RESTORE]]))

    crit_d, resume_d, full_d = CHEM_DEFAULTS[chem]

    async def param(key, method, default=None):
        value = config.get(key, default)
        if value is None:
            return
        if isinstance(value, ID):
            n = await cg.get_variable(value)
            cg.add(cg.RawStatement(f"{var}->{method}().num = {n};"))
        else:
            cg.add(cg.RawStatement(f"{var}->{method}().value = {float(value)}f;"))

    await param(CONF_CAPACITY, "capacity")
    await param(CONF_INTERNAL_RESISTANCE, "internal_resistance")
    await param(CONF_CELLS_SERIES, "cells_series")
    await param(CONF_CELLS_PARALLEL, "cells_parallel")
    await param(CONF_LOW, "low")
    await param(CONF_CRITICAL, "critical", crit_d)
    await param(CONF_RESUME, "resume", resume_d)

    smoothing = config.get(CONF_SMOOTHING, 1.0 if has_current else 0.15)
    only_falls = config.get(CONF_ONLY_FALLS, not has_current)
    cg.add(cg.RawStatement(f"{var}->core_config().smoothing = {smoothing}f;"))
    cg.add(cg.RawStatement(f"{var}->core_config().only_falls = {str(only_falls).lower()};"))
    cg.add(cg.RawStatement(f"{var}->core_config().round_percent = {str(not has_current).lower()};"))
    cg.add(cg.RawStatement(f"{var}->core_config().jump_up_v = {config[CONF_JUMP_UP]}f;"))
    cg.add(cg.RawStatement(f"{var}->core_config().report_step_v = {config[CONF_REPORT_STEP]}f;"))
    cg.add(cg.RawStatement(f"{var}->core_config().report_avg_n = {config[CONF_REPORT_AVERAGE]};"))
    if config[CONF_CHARGE_DETECT]:
        cg.add(cg.RawStatement(f"{var}->core_config().charge_detect = true;"))
        charge_v = config.get(CONF_CHARGE_VOLTAGE, CHARGE_V_DEFAULTS[chem])
        cg.add(cg.RawStatement(f"{var}->core_config().charge_v = {charge_v}f;"))
        cg.add(cg.RawStatement(f"{var}->core_config().charge_jump_v = {config[CONF_CHARGE_JUMP]}f;"))
    cg.add(cg.RawStatement(f"{var}->core_config().charge_enter_a = {config[CONF_CHARGE_ENTER_CURRENT]}f;"))
    cg.add(cg.RawStatement(f"{var}->core_config().charge_exit_a = {config[CONF_CHARGE_EXIT_CURRENT]}f;"))
    cg.add(cg.RawStatement(f"{var}->core_config().discharge_a = {config[CONF_DISCHARGE_CURRENT]}f;"))
    cg.add(cg.RawStatement(f"{var}->core_config().debounce = {config[CONF_DEBOUNCE]};"))
    cg.add(cg.RawStatement(f"{var}->core_config().full_v = {config.get(CONF_FULL_VOLTAGE, full_d)}f;"))
    cg.add(cg.RawStatement(f"{var}->core_config().full_a = {config[CONF_FULL_CURRENT]}f;"))
    cg.add(
        cg.RawStatement(
            f"{var}->core_config().hold_after_charge_ms = {config[CONF_HOLD_AFTER_CHARGE].total_milliseconds}UL;"
        )
    )

    if CONF_PERCENT in config:
        cg.add(var.set_percent_sensor(await sensor.new_sensor(config[CONF_PERCENT])))
    if CONF_REST_VOLTAGE in config:
        cg.add(var.set_voltage_out_sensor(await sensor.new_sensor(config[CONF_REST_VOLTAGE])))
    if CONF_REPORT_VOLTAGE in config:
        cg.add(var.set_report_voltage_sensor(await sensor.new_sensor(config[CONF_REPORT_VOLTAGE])))
    if CONF_REMAINING in config:
        cg.add(var.set_remaining_sensor(await sensor.new_sensor(config[CONF_REMAINING])))
    if CONF_TIME_LEFT in config:
        cg.add(var.set_time_left_sensor(await sensor.new_sensor(config[CONF_TIME_LEFT])))
    if "power_out" in config:
        cg.add(var.set_power_out_sensor(await sensor.new_sensor(config["power_out"])))
    if CONF_CHARGING in config:
        cg.add(var.set_charging_binary_sensor(await binary_sensor.new_binary_sensor(config[CONF_CHARGING])))
    if CONF_LOW_ALARM in config:
        cg.add(var.set_low_binary_sensor(await binary_sensor.new_binary_sensor(config[CONF_LOW_ALARM])))
    if CONF_CRITICAL_ALARM in config:
        cg.add(var.set_critical_binary_sensor(await binary_sensor.new_binary_sensor(config[CONF_CRITICAL_ALARM])))
    if CONF_STATUS in config:
        cg.add(var.set_status_text_sensor(await text_sensor.new_text_sensor(config[CONF_STATUS])))
