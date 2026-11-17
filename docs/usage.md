# Using li_battery

## 1. Hello world

The smallest config that works — a template sensor stands in for your real voltage
([`examples/hello_world.yaml`](../examples/hello_world.yaml)):

```yaml
external_components:
  - source: github://daimonkor/esphome-li-battery@v1.0.0
    components: [li_battery]

sensor:
  - platform: template
    id: battery_voltage
    unit_of_measurement: V
    update_interval: 10s
    lambda: return 3.85;          # your real sensor goes here

li_battery:
  voltage: battery_voltage
  chemistry: lipo
  percent:
    name: Battery
```

Log after flashing:

```
[C][li_battery]: Li battery:
[C][li_battery]:   Chemistry: lipo (21 curve points), 1S1P
[C][li_battery]:   Mode: voltage only
[D][li_battery]: V 3.850 I nan -> rest 3.850 V, 55.0 %
```

## 2. Pick the mode by what you measure

| You have | Give li_battery | You get |
|---|---|---|
| ADC + divider | `voltage` | percent, low / critical, averaged voltage |
| INA226 / INA219 (V and I) | `voltage`, `current` | + charging, status, coulomb counting, time left, power |
| V and signed power | `voltage`, `power` | same, I = P / V |
| your own rest-voltage logic | nothing — call `id(bat).process_voltage(v)` | percent, levels |

**Your sensor must already give volts / amperes / watts.** The divider factor, shunt, offsets — all of
that stays in your sensor (`filters: multiply: 2.03`). Current and power: **+ discharge, − charge**.

## 3. Measure voltage at rest

Voltage under load is lower and the percent jumps. Voltage only mode works best with a reading taken
before Wi-Fi / BLE start, e.g. in `on_boot` with priority 550:

```yaml
esphome:
  on_boot:
    - priority: 550                # ADC is up, radio is not
      then:
        - component.update: vbat

sensor:
  - platform: adc
    id: vbat
    pin: GPIO2
    attenuation: 12db
    samples: 64
    update_interval: never
    filters:
      - multiply: 2.03

li_battery:
  id: bat
  voltage: vbat
  chemistry: lipo
  restore: rtc                     # deep sleep: keep the history
```

With current, `internal_resistance` does this for you: OCV = V + I·R.

## 4. Settings from Home Assistant

Any number setting takes a value or the id of a `number`:

```yaml
number:
  - platform: template             # your own number
    id: cap
    optimistic: true
    restore_value: true
    min_value: 0
    max_value: 30000
    step: 100
    initial_value: 2900

li_battery:
  capacity: cap                    # follows the number
```

or let li_battery create it (kept in flash, shown in HA as a config entity):

```yaml
number:
  - platform: li_battery
    li_battery_id: bat
    type: capacity                 # capacity, internal_resistance, cells_series,
    name: Battery Capacity         # cells_parallel, low, critical, resume
    initial_value: 2900
```

## 5. Use it in your logic

```yaml
interval:
  - interval: 1min
    then:
      - if:
          condition:
            lambda: return id(bat).is_critical();
          then:
            - deep_sleep.enter: {id: ds, sleep_duration: 6h}

script:
  - id: modem_on
    then:
      - if:
          condition:
            # 30 % on the curve, as a voltage — e.g. a GSM cut-off
            lambda: return id(bat_voltage).state > id(bat).voltage_for_percent(30);
          then:
            - switch.turn_on: modem_power
```

Available: `percent()`, `is_low()`, `is_critical()`, `is_charging()`, `rest_voltage()`,
`report_voltage()`, `remaining_mah()`, `voltage_for_percent(p)`, `process_voltage(v)`,
`rescale(ratio)`, `reset()`.

## 6. Divider factor changed from HA

The smoothed history was computed with the old factor. Tell li_battery, and the percent moves to the
new value at once instead of drifting over several readings:

```yaml
number:
  - platform: template
    id: vbat_factor
    # ...
    set_action:
      - lambda: |-
          if (id(vbat_factor).state > 0) id(bat).rescale(x / id(vbat_factor).state);
```

## 7. Packs

| Pack | Config |
|---|---|
| one LiPo 920 mAh | `chemistry: lipo`, `capacity: 920` |
| 3 × 18650 2900 mAh in parallel | `chemistry: liion`, `cells_parallel: 3`, `capacity: 2900` |
| 2S Li-ion | `chemistry: liion`, `cells_series: 2` — voltage is the whole pack |
| 4S LiFePO4 | `chemistry: lifepo4`, `cells_series: 4` |
| your own cell | `chemistry: custom`, `curve: [{voltage: 3.0V, percent: 0}, …]` |

## 8. Check it

```bash
make -C tests/host          # core: curves, both modes, equivalence with the code it replaced
python3 -m pytest tests/config   # ESPHome config validation, good and bad configs
```

How it works inside, with diagrams: [how-it-works.md](how-it-works.md).
