# Changelog

## 1.1.0 — 2026-11-17

- `voltage_for_percent(pct, current)`: with a current, the voltage a meter shows at that load
  (OCV − I·R) — e.g. a modem cut-off taken from a percent.
- Examples for the two real devices it runs on: `move_sensor_a0.yaml` (only A0 with a divider, deep
  sleep, `process_voltage`, divider factor and battery settings from HA) and `hivescale_ina226.yaml`
  (INA226 voltage + current + power, 3 × 18650, capacity number with 0 = not set). Both are built in CI.

## 1.0.0 — 2026-11-17

First release.

- State of charge for LiPo, Li-ion (18650 / 21700), LiFePO4 or a custom curve.
- Voltage only: smoothing, percent only falls unless charging, low / critical levels, averaged voltage for reports.
- Voltage + current or power (INA226 and the like): I·R compensation, charging by the current sign with
  hysteresis and debounce, coulomb counting while charging and 20 min after it.
- Cells in series / parallel, capacity in mAh per cell.
- Every setting can follow a `number` entity; `number: platform: li_battery` creates them (kept in flash).
- `restore: rtc` keeps the state through deep sleep.
- Outputs: percent, rest voltage, report voltage, remaining mAh, time left, power, charging, low, critical, status.
