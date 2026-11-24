# esphome-li-battery

**English** · [Русский](README.ru.md) · [Українська](README.uk.md)

ESPHome external component: state of charge for **LiPo / Li-ion (18650, 21700) / LiFePO4** batteries.

- **Voltage only** — an ADC divider measured at rest (before the radio starts). Smoothing, the percent
  only falls unless the battery is charging, low / critical levels that survive deep sleep.
  With `charge_detect: true` it also tells when the battery is on the charger (no charge-status pin
  needed) and how fast it discharges, so `time_left` works without a current sensor.
- **Voltage + current (or power)** — INA226 and the like. Open-circuit voltage `V + I·R`, charging
  detected by the current sign with hysteresis and debounce, coulomb counting while charging and
  for 20 min after it, then back to the voltage curve.

`chemistry` only picks the curve and default levels — the mode follows the sensors you give. All inputs are your own sensors; every setting is a number or the id of a `number` entity, so it can
be changed from Home Assistant.

```yaml
external_components:
  - source: github://daimonkor/esphome-li-battery@v1.2.1
    components: [li_battery]
```

**Docs:** [how to use](docs/usage.md) · [how it works, with diagrams](docs/how-it-works.md)

## Hello world

```yaml
sensor:
  - platform: template
    id: battery_voltage
    unit_of_measurement: V
    lambda: return 3.85;          # your real sensor goes here (adc, ina226, ...)

li_battery:
  voltage: battery_voltage        # volts, divider already applied
  chemistry: lipo
  percent:
    name: Battery                 # -> 55 %
```

## Voltage only (LiPo on an ADC divider)

```yaml
sensor:
  - platform: adc
    id: vbat
    pin: GPIO2
    attenuation: 12db
    samples: 64
    filters:
      - multiply: 2.03

li_battery:
  id: bat
  voltage: vbat
  chemistry: lipo
  capacity: 920          # mAh
  restore: rtc           # keep the history through deep sleep
  percent:
    name: Battery
  low_alarm:
    name: Battery low
```

No input sensor at all is fine too — feed the rest voltage yourself:
`id(bat).process_voltage(v)` returns the percent.

## Voltage + current (INA226, 3 × 18650 in parallel)

```yaml
li_battery:
  id: bat
  voltage: bat_voltage   # V
  current: bat_current   # A, + discharge, − charge   (or  power: W, same sign)
  chemistry: liion
  cells_parallel: 3
  capacity: 2900         # mAh per cell -> 8700 mAh
  internal_resistance: 0.14
  percent:
    name: Battery Level
  charging:
    name: Battery Charging
  status:
    name: Battery Status   # Charging / Discharging / Idle
  time_left:
    name: Battery Time Left
  power_out:
    name: Battery Power

number:
  - platform: li_battery   # or capacity: <id of your own number>
    li_battery_id: bat
    type: capacity
    name: Battery Capacity
    initial_value: 2900
```

More in [`examples/`](examples) — including the two real devices li_battery runs on:
[`move_sensor_a0.yaml`](examples/move_sensor_a0.yaml) (only A0 with a divider) and
[`hivescale_ina226.yaml`](examples/hivescale_ina226.yaml) (INA226, 3 × 18650).

## Configuration

**Inputs**

| Key | | |
|---|---|---|
| `voltage` | sensor id | pack voltage, V. Optional if you call `process_voltage()` |
| `current` | sensor id | A, **+ discharge, − charge** |
| `power` | sensor id | W, same sign. With `current` too: passed to `power_out` as is; without `current`: I = P / V |

**Battery** — values marked *(n)* take a number or the id of a `number` entity

| Key | Default | |
|---|---|---|
| `chemistry` | `lipo` | `lipo` (3.30–4.14 V) · `liion` (2.90–4.20 V) · `lifepo4` (2.50–3.45 V) · `custom` |
| `curve` | — | `custom` only: list of `{voltage: 3.7V, percent: 50}`, per cell |
| `cells_series` *(n)* | 1 | cells in series — the pack voltage is divided by this |
| `cells_parallel` *(n)* | 1 | cells in parallel — capacity is multiplied by this |
| `capacity` *(n)* | — | mAh **per cell** (a plain number, always mAh). If your number already holds the whole pack, keep `cells_parallel: 1` |
| `internal_resistance` *(n)* | 0 | pack, Ω — `OCV = V + I·R` |
| `restore` | `none` | `rtc`: history and levels survive deep sleep (ESP32, up to 2 instances) |
| `low` *(n)* | 10 | %, clears 5 % higher |
| `critical` / `resume` *(n)* | by chemistry | V per cell, rest. lipo 3.50 / 3.65 · liion 3.20 / 3.40 · lifepo4 2.90 / 3.10 |

**Voltage only tuning:** `smoothing` (0.15, 1 = off), `only_falls` (true), `jump_up` (0.10 V per cell — a
rise this big is a charger and is taken at once), `report_average` (10), `report_step` (0.03 V).

**Charging without a current sensor** (voltage only): `charge_detect: true`. Charging starts when two
readings in a row are at or above `charge_voltage` (lipo / liion 4.165 V per cell, lifepo4 3.50 V) or
`charge_jump` (0.04 V per cell) above the smoothed rest voltage; a jump that does not keep climbing
within 30 min (a rebound after a load) is dropped. It ends when two readings in a row are under
`charge_voltage` and 0.05 V below the peak on the charger, or when the readings have not touched the top of
the charge (4.195 V per cell) for 10 min — unplugged near full, or the charger has finished. The discharge rate is taken from the percent
drop since the last charge (at least 2 % over 12 h) and gives `time_left`. Needs a clock that runs
through deep sleep (ESP32 RTC — built in).

**Current tuning (A):** `charge_enter_current` (−0.008), `charge_exit_current` (−0.004),
`discharge_current` (0.008), `debounce` (2 updates), `full_voltage` (4.15 V per cell, by chemistry),
`full_current` (−0.05: above it at full voltage = 100 %), `hold_after_charge` (20min),
`update_interval` (2s). Charging strictly by the sign: `charge_enter_current: 0`,
`charge_exit_current: 0`, `debounce: 1`.

**Outputs** (all optional): `percent`, `rest_voltage` (smoothed / I·R-compensated), `report_voltage`
(average of the last readings, changes in 0.03 V steps — good for BLE packets), `remaining` (mAh),
`time_left` (h, while discharging; voltage only — with `charge_detect`), `power_out` (W), `charging`, `low_alarm`, `critical_alarm`
(binary), `status` (text).

**In lambdas:** `id(bat).percent()`, `is_low()`, `is_critical()`, `is_charging()`, `rest_voltage()`,
`report_voltage()`, `remaining_mah()`, `discharge_rate_pct_h()`, `time_left_h()`, `voltage_for_percent(30, current)` (pack voltage for a percent, with a current — what a meter
shows under that load; e.g. a modem cut-off), `process_voltage(v)`, `rescale(ratio)` (divider factor changed), `reset()`.

## Tests

The core (`li_battery_core.h`) has no ESPHome dependency.

- `make -C tests/host` — ~3 million checks: scenarios with explicit expectations, plus equivalence runs
  against the code li_battery replaced in two real devices (a BLE motion sensor on a LiPo and a solar
  beehive scale with an INA226 and 18650 cells);
- `make -C tests/host replay` — 6 days of real INA226 data from the scale: 26 080 ticks, the same result as
  the scale's own code on every tick, within 0.74 % of what it reported to Home Assistant;
- `python3 -m pytest tests/config` — good and bad ESPHome configs.

CI runs all of it and builds the examples. Also tested on a real device.

## Changelog

[CHANGELOG.md](CHANGELOG.md)
