# Как устроена li_battery

[English](how-it-works.md) · **Русский** · [Українська](how-it-works.uk.md)

## Модель

```mermaid
classDiagram
  direction LR
  class LiBattery {
    <<компонент ESPHome>>
    voltage : Sensor
    current : Sensor, необязательно
    power : Sensor, необязательно
    capacity, internal_resistance,
    cells_series, cells_parallel,
    low, critical, resume : Param
    restore : none | rtc
    setup()
    update() раз в update_interval, если есть ток или мощность
    process_voltage(v) : процент
    rescale(ratio)
    reset()
  }
  class Param {
    value : float
    num : number::Number, необязательно
    get() : значение number, если задан, иначе value
  }
  class Estimator {
    <<li_battery_core.h — без ESPHome>>
    cfg : Config
    st : State*
    update_voltage(v) : процент
    update(v, i, now_ms) : процент
    rescale(ratio)
    remaining_mah()
    time_left_h(i)
    report_voltage()
  }
  class Config {
    curve : Curve
    банки, ёмкость, R
    smoothing, only_falls, jump_up
    low / critical / resume
    charge_enter / exit, debounce
    full_v / full_a, hold_after_charge
  }
  class State {
    <<RTC-память при restore: rtc>>
    vf, pct_last, кольцо для отчёта
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
  LiBattery ..> "выходы" Sensor : percent, rest_voltage, report_voltage,<br/>remaining, time_left, power_out
  LiBattery ..> "выходы" BinarySensor : charging, low_alarm, critical_alarm
  LiBattery ..> "выходы" TextSensor : status
```

Режим определяется входами: только `voltage` — «только напряжение»; есть ещё `current` и/или `power` —
«напряжение + ток». `chemistry` только выбирает кривую и пороги по умолчанию.

## Только напряжение

```mermaid
sequenceDiagram
  participant ADC as Твой сенсор напряжения (АЦП × делитель)
  participant LB as LiBattery
  participant E as Estimator (ядро)
  participant S as State (RTC)
  participant HA as Выходы → Home Assistant
  Note over LB,S: setup(): State из RTC (magic совпал) или с нуля
  ADC->>LB: publish_state(V)  [on_state]
  LB->>LB: применить настройки (number могли поменяться)
  LB->>E: update_voltage(V)
  E->>S: кольцо для отчёта: среднее 10 последних, новое значение только при изменении ≥ 0.03 В
  E->>S: vf = EMA(V, 0.15) — или сразу V при подъёме > 0.10 В (зарядка) / на первом замере
  E->>E: процент = round(кривая(vf))
  E->>E: только падает: подъём < 10 % без скачка игнорируется
  E->>S: low (≤ 10 %, снимается от 15 %), critical (vf ≤ critical, снимается при resume)
  E-->>LB: процент
  LB->>HA: percent, rest_voltage, report_voltage, remaining, low_alarm, critical_alarm
  Note over ADC,LB: Или без сенсора: твоя лямбда вызывает id(bat).process_voltage(V)
```

## Напряжение + ток

```mermaid
sequenceDiagram
  participant INA as Твои сенсоры (V, I и/или P)
  participant LB as LiBattery
  participant E as Estimator (ядро)
  participant S as State
  participant HA as Выходы → Home Assistant
  loop раз в update_interval (2 с)
    LB->>INA: прочитать V, I (или I = P / V)
    LB->>E: update(V, I, now)
    E->>E: OCV = V + I·R  →  soc_ocv = кривая(OCV / cells_series)
    E->>S: зарядка? вход при I < −8 мА, выход при > −4 мА, 2 замера подряд
    alt зарядка
      E->>S: старт: soc_cc = последний % в покое (без скачка), дальше += −I·dt / ёмкость
      E->>S: V ≥ full_v и I > −50 мА → 100 %
    else не заряжается, первые 20 мин после зарядки
      E->>S: счёт продолжается (напряжение после зарядки ещё завышено)
    else покой / разряд
      E->>S: soc_cc выключен, last_idle_soc = soc_ocv
    end
    E-->>LB: процент = soc_cc, если идёт счёт, иначе soc_ocv
    LB->>HA: percent, charging, status, remaining, time_left, power_out, low/critical
  end
```

## Зарядка и счёт кулонов

```mermaid
stateDiagram-v2
  state "Покой" as Rest
  state "Зарядка" as Charging
  state "Удержание" as Hold
  [*] --> Rest
  Rest --> Charging : I < charge_enter debounce замеров подряд<br/>(soc_cc = last_idle_soc)
  Charging --> Charging : soc_cc += −I·dt / ёмкость<br/>V ≥ full_v и I > full_a → 100 %
  Charging --> Hold : I > charge_exit debounce замеров подряд
  Hold --> Charging : снова зарядка (счёт сохраняется)
  Hold --> Rest : прошло hold_after_charge (20 мин)<br/>(снова по кривой напряжения)
  Rest --> Rest : процент = кривая(V + I·R)<br/>обновляется last_idle_soc
```

## Уровни

```mermaid
stateDiagram-v2
  direction LR
  state "Норма" as Normal
  state "low" as L
  state "critical" as C
  Normal --> L : процент ≤ low
  L --> Normal : процент ≥ low + 5
  Normal --> C : покой, В на банку ≤ critical
  L --> C : покой, В на банку ≤ critical
  C --> Normal : покой, В на банку ≥ resume (подзарядили)
```

## Единицы и знаки

| | |
|---|---|
| Напряжение | В, вся сборка, делитель уже учтён твоим сенсором |
| Ток | А, **+ разряд, − заряд** |
| Мощность | Вт, тот же знак |
| Ёмкость | мА·ч **на одну банку** (× `cells_parallel`) |
| Внутреннее сопротивление | Ом, вся сборка |
| critical / resume / full | В **на одну банку** (в покое) |
