# Changelog

## 1.2.1 — 2026-11-24

- `charge_detect`: charging also ends when the readings have not touched the top of the charge
  (4.195 V per cell) for 10 min and 2 readings. A LiPo taken off the charger near full rests at
  4.16–4.18 V — above the 1.2.0 end line (4.155 V) — and stayed "charging" for hours; a charger that
  has finished is now "not charging" too. Seen on the Move Sensor: on the charger the readings touch
  4.20–4.21 V every 30–80 s, unplugged they do not.
- RTC state magic `LiB3` (struct grew): the deep-sleep state starts over once after the update.

## 1.2.0 — 2026-11-24

- Voltage only: `charge_detect: true` — tells when the battery is on the charger without a
  charge-status pin or a current sensor. Starts on two readings at or above `charge_voltage`
  (lipo / liion 4.165 V per cell) or `charge_jump` (0.04 V) above the smoothed rest voltage; a jump
  that does not keep climbing within 30 min (rebound after a load) is dropped; ends on two readings
  under `charge_voltage` and 0.05 V below the peak. Tuned on a month of Move Sensor BLE history:
  4.17–4.21 V on the charger, 4.15 V at rest right after it (not charging).
- On the charger the percent follows the reading at once; after it the history restarts from the
  first rest reading (no charger voltage left in the average).
- Discharge rate since the last charge (≥ 2 % over ≥ 12 h) → `time_left` and `time_left_h()`
  without a current sensor; `discharge_rate_pct_h()` in lambdas. `charging` and `status` outputs now
  work in voltage-only mode with `charge_detect`.
- RTC state magic is now `LiB2`: after an update the old deep-sleep state is dropped once (the struct grew).

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
