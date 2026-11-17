# Як влаштована li_battery

[English](how-it-works.md) · [Русский](how-it-works.ru.md) · **Українська**

## Модель

```mermaid
classDiagram
  direction LR
  class LiBattery {
    <<компонент ESPHome>>
    voltage : Sensor
    current : Sensor, необов'язково
    power : Sensor, необов'язково
    capacity, internal_resistance,
    cells_series, cells_parallel,
    low, critical, resume : Param
    restore : none | rtc
    setup()
    update() раз на update_interval, якщо є струм або потужність
    process_voltage(v) : відсоток
    rescale(ratio)
    reset()
  }
  class Param {
    value : float
    num : number::Number, необов'язково
    get() : значення number, якщо задано, інакше value
  }
  class Estimator {
    <<li_battery_core.h — без ESPHome>>
    cfg : Config
    st : State*
    update_voltage(v) : відсоток
    update(v, i, now_ms) : відсоток
    rescale(ratio)
    remaining_mah()
    time_left_h(i)
    report_voltage()
  }
  class Config {
    curve : Curve
    банки, ємність, R
    smoothing, only_falls, jump_up
    low / critical / resume
    charge_enter / exit, debounce
    full_v / full_a, hold_after_charge
  }
  class State {
    <<RTC-пам'ять за restore: rtc>>
    vf, pct_last, кільце для звіту
    low, critical
    charging, soc_cc, last_idle_soc
    charge_end_ms, last_ms
    percent, ocv, status
  }
  class Curve {
    CurvePoint[] (В на банку -> %)
    lipo | liion | lifepo4 | custom
  }
  LiBattery *-- Estimator
  LiBattery *-- "7" Param
  Estimator o-- Config
  Estimator --> State
  Config o-- Curve
  LiBattery ..> "виходи" Sensor : percent, rest_voltage, report_voltage,<br/>remaining, time_left, power_out
  LiBattery ..> "виходи" BinarySensor : charging, low_alarm, critical_alarm
  LiBattery ..> "виходи" TextSensor : status
```

Режим визначається входами: лише `voltage` — «лише напруга»; є ще `current` і/або `power` —
«напруга + струм». `chemistry` лише обирає криву та пороги за замовчуванням.

## Лише напруга

```mermaid
sequenceDiagram
  participant ADC as Твій сенсор напруги (АЦП × дільник)
  participant LB as LiBattery
  participant E as Estimator (ядро)
  participant S as State (RTC)
  participant HA as Виходи → Home Assistant
  Note over LB,S: setup(): State з RTC (magic збігся) або з нуля
  ADC->>LB: publish_state(V)  [on_state]
  LB->>LB: застосувати налаштування (number могли змінитися)
  LB->>E: update_voltage(V)
  E->>S: кільце для звіту: середнє 10 останніх, нове значення лише при зміні ≥ 0.03 В
  E->>S: vf = EMA(V, 0.15) — або одразу V при підйомі > 0.10 В (заряджання) / на першому вимірі
  E->>E: відсоток = round(крива(vf))
  E->>E: лише падає: підйом < 10 % без стрибка ігнорується
  E->>S: low (≤ 10 %, знімається від 15 %), critical (vf ≤ critical, знімається при resume)
  E-->>LB: відсоток
  LB->>HA: percent, rest_voltage, report_voltage, remaining, low_alarm, critical_alarm
  Note over ADC,LB: Або без сенсора: твоя лямбда викликає id(bat).process_voltage(V)
```

## Напруга + струм

```mermaid
sequenceDiagram
  participant INA as Твої сенсори (V, I і/або P)
  participant LB as LiBattery
  participant E as Estimator (ядро)
  participant S as State
  participant HA as Виходи → Home Assistant
  loop раз на update_interval (2 с)
    LB->>INA: прочитати V, I (або I = P / V)
    LB->>E: update(V, I, now)
    E->>E: OCV = V + I·R  →  soc_ocv = крива(OCV / cells_series)
    E->>S: заряджання? вхід при I < −8 мА, вихід при > −4 мА, 2 виміри поспіль
    alt заряджання
      E->>S: старт: soc_cc = останній % у спокої (без стрибка), далі += −I·dt / ємність
      E->>S: V ≥ full_v і I > −50 мА → 100 %
    else не заряджається, перші 20 хв після заряджання
      E->>S: лічба триває (напруга після заряджання ще завищена)
    else спокій / розряд
      E->>S: soc_cc вимкнено, last_idle_soc = soc_ocv
    end
    E-->>LB: відсоток = soc_cc, якщо йде лічба, інакше soc_ocv
    LB->>HA: percent, charging, status, remaining, time_left, power_out, low/critical
  end
```

## Заряджання та лічба кулонів

```mermaid
stateDiagram-v2
  state "Спокій" as Rest
  state "Заряджання" as Charging
  state "Утримання" as Hold
  [*] --> Rest
  Rest --> Charging : I < charge_enter debounce вимірів поспіль<br/>(soc_cc = last_idle_soc)
  Charging --> Charging : soc_cc += −I·dt / ємність<br/>V ≥ full_v і I > full_a → 100 %
  Charging --> Hold : I > charge_exit debounce вимірів поспіль
  Hold --> Charging : знову заряджання (лічба зберігається)
  Hold --> Rest : минуло hold_after_charge (20 хв)<br/>(знову за кривою напруги)
  Rest --> Rest : відсоток = крива(V + I·R)<br/>оновлюється last_idle_soc
```

## Рівні

```mermaid
stateDiagram-v2
  direction LR
  state "Норма" as Normal
  state "low" as L
  state "critical" as C
  Normal --> L : відсоток ≤ low
  L --> Normal : відсоток ≥ low + 5
  Normal --> C : спокій, В на банку ≤ critical
  L --> C : спокій, В на банку ≤ critical
  C --> Normal : спокій, В на банку ≥ resume (підзарядили)
```

## Одиниці та знаки

| | |
|---|---|
| Напруга | В, уся збірка, дільник уже враховано твоїм сенсором |
| Струм | А, **+ розряд, − заряд** |
| Потужність | Вт, той самий знак |
| Ємність | мА·год **на одну банку** (× `cells_parallel`) |
| Внутрішній опір | Ом, уся збірка |
| critical / resume / full | В **на одну банку** (у спокої) |
