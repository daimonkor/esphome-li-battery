# How li_battery works

**English** · [Русский](how-it-works.ru.md) · [Українська](how-it-works.uk.md)

## Model

```mermaid
classDiagram
  direction LR
  class LiBattery {
    <<ESPHome component>>
    voltage : Sensor
    current : Sensor, optional
    power : Sensor, optional
    capacity, internal_resistance,
    cells_series, cells_parallel,
    low, critical, resume : Param
    restore : none | rtc
    setup()
    update() every update_interval, with current or power
    process_voltage(v) : percent
    rescale(ratio)
    reset()
  }
  class Param {
    value : float
    num : number::Number, optional
    get() : number state if set, else value
  }
  class Estimator {
    <<li_battery_core.h — no ESPHome>>
    cfg : Config
    st : State*
    update_voltage(v) : percent
    update(v, i, now_ms) : percent
    rescale(ratio)
    remaining_mah()
    time_left_h(i)
    report_voltage()
  }
  class Config {
    curve : Curve
    cells, capacity, R
    smoothing, only_falls, jump_up
    low / critical / resume
    charge_enter / exit, debounce
    full_v / full_a, hold_after_charge
  }
  class State {
    <<RTC memory with restore: rtc>>
    vf, pct_last, report ring
    low, critical
    charging, soc_cc, last_idle_soc
    charge_end_ms, last_ms
    percent, ocv, status
  }
  class Curve {
    CurvePoint[] (V per cell -> %)
    lipo | liion | lifepo4 | custom
  }
  LiBattery *-- Estimator
  LiBattery *-- "7" Param
  Estimator o-- Config
  Estimator --> State
  Config o-- Curve
  LiBattery ..> "outputs" Sensor : percent, rest_voltage, report_voltage,<br/>remaining, time_left, power_out
  LiBattery ..> "outputs" BinarySensor : charging, low_alarm, critical_alarm
  LiBattery ..> "outputs" TextSensor : status
```

The mode follows the inputs: only `voltage` → voltage-only; `current` and/or `power` too → voltage + current.
`chemistry` only picks the curve and the default levels.

## Voltage only

```mermaid
sequenceDiagram
  participant ADC as Your voltage sensor (ADC × divider)
  participant LB as LiBattery
  participant E as Estimator (core)
  participant S as State (RTC)
  participant HA as Outputs → Home Assistant
  Note over LB,S: setup(): State from RTC (magic ok) or fresh
  ADC->>LB: publish_state(V)  [on_state callback]
  LB->>LB: apply params (numbers may have changed)
  LB->>E: update_voltage(V)
  E->>S: report ring: average of last 10, new value only on ≥ 0.03 V change
  E->>S: vf = EMA(V, 0.15) — or V at once on a rise > 0.10 V (charger) / first reading
  E->>E: percent = round(curve(vf))
  E->>E: only falls: a rise < 10 % without a jump is ignored
  E->>S: low (≤ 10 %, clears 15 %), critical (vf ≤ critical, clears at resume)
  E-->>LB: percent
  LB->>HA: percent, rest_voltage, report_voltage, remaining, low_alarm, critical_alarm
  Note over ADC,LB: Or no sensor: your lambda calls id(bat).process_voltage(V)
```

## Voltage + current

```mermaid
sequenceDiagram
  participant INA as Your sensors (V, I and/or P)
  participant LB as LiBattery
  participant E as Estimator (core)
  participant S as State
  participant HA as Outputs → Home Assistant
  loop every update_interval (2 s)
    LB->>INA: read V, I (or I = P / V)
    LB->>E: update(V, I, now)
    E->>E: OCV = V + I·R  →  soc_ocv = curve(OCV / cells_series)
    E->>S: charging? I < −8 mA to enter, > −4 mA to leave, 2 updates in a row
    alt charging
      E->>S: start: soc_cc = last rest % (no jump), then += −I·dt / capacity
      E->>S: V ≥ full_v and I > −50 mA → 100 %
    else not charging, within 20 min after charge
      E->>S: keep counting (voltage is still too high after charging)
    else rest / discharge
      E->>S: soc_cc = off, last_idle_soc = soc_ocv
    end
    E-->>LB: percent = soc_cc if counting else soc_ocv
    LB->>HA: percent, charging, status, remaining, time_left, power_out, low/critical
  end
```

## Charging and coulomb counting

```mermaid
stateDiagram-v2
  [*] --> Rest
  Rest --> Charging : I < charge_enter for debounce updates<br/>(soc_cc = last_idle_soc)
  Charging --> Charging : soc_cc += −I·dt / capacity<br/>V ≥ full_v & I > full_a → 100 %
  Charging --> Hold : I > charge_exit for debounce updates
  Hold --> Charging : charging again (count kept)
  Hold --> Rest : hold_after_charge (20 min) passed<br/>(back to the voltage curve)
  Rest --> Rest : percent = curve(V + I·R)<br/>last_idle_soc updated
```

## Levels

```mermaid
stateDiagram-v2
  direction LR
  state "low" as L
  state "critical" as C
  Normal --> L : percent ≤ low
  L --> Normal : percent ≥ low + 5
  Normal --> C : rest V per cell ≤ critical
  L --> C : rest V per cell ≤ critical
  C --> Normal : rest V per cell ≥ resume (recharged)
```

## Units and signs

| | |
|---|---|
| Voltage | V, whole pack, divider already applied by your sensor |
| Current | A, **+ discharge, − charge** |
| Power | W, same sign |
| Capacity | mAh **per cell** (× `cells_parallel`) |
| Internal resistance | Ω, whole pack |
| critical / resume / full | V **per cell** (rest) |
