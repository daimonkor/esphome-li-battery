# esphome-li-battery

[English](README.md) · **Русский** · [Українська](README.uk.md)

Внешний компонент ESPHome: заряд батарей **LiPo / Li-ion (18650, 21700) / LiFePO4**.

- **Только напряжение** — делитель на АЦП, замер в покое (до запуска радио). Сглаживание, процент
  без зарядки только падает, уровни «низкий» / «критический» переживают deep sleep.
- **Напряжение + ток (или мощность)** — INA226 и подобные. Напряжение покоя `V + I·R`, зарядка по
  знаку тока с гистерезисом и задержкой, счёт кулонов на зарядке и 20 мин после неё, потом снова
  по кривой напряжения.

`chemistry` только выбирает кривую и пороги по умолчанию — режим определяется тем, какие сенсоры
ты передал. Все входы — твои сенсоры; любая настройка — число или id `number`, поэтому её можно
менять из Home Assistant.

```yaml
external_components:
  - source: github://daimonkor/esphome-li-battery@v1.2.0
    components: [li_battery]
```

**Документация:** [как пользоваться](docs/usage.ru.md) · [как устроено, с диаграммами](docs/how-it-works.ru.md)

## Hello world

```yaml
sensor:
  - platform: template
    id: battery_voltage
    unit_of_measurement: V
    lambda: return 3.85;          # здесь твой настоящий сенсор (adc, ina226, ...)

li_battery:
  voltage: battery_voltage        # вольты, делитель уже учтён
  chemistry: lipo
  percent:
    name: Battery                 # -> 55 %
```

## Только напряжение (LiPo на делителе АЦП)

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
  capacity: 920          # мА·ч
  restore: rtc           # история переживает deep sleep
  percent:
    name: Battery
  low_alarm:
    name: Battery low
```

Можно и без входного сенсора — отдавай напряжение покоя сам:
`id(bat).process_voltage(v)` возвращает процент.

## Напряжение + ток (INA226, 3 × 18650 параллельно)

```yaml
li_battery:
  id: bat
  voltage: bat_voltage   # В
  current: bat_current   # А, + разряд, − заряд   (или  power: Вт, тот же знак)
  chemistry: liion
  cells_parallel: 3
  capacity: 2900         # мА·ч на банку -> 8700 мА·ч
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
  - platform: li_battery   # или capacity: <id своего number>
    li_battery_id: bat
    type: capacity
    name: Battery Capacity
    initial_value: 2900
```

Ещё примеры — в [`examples/`](examples), в том числе два реальных устройства:
[`move_sensor_a0.yaml`](examples/move_sensor_a0.yaml) (только A0 с делителем) и
[`hivescale_ina226.yaml`](examples/hivescale_ina226.yaml) (INA226, 3 × 18650).

## Настройки

**Входы**

| Ключ | | |
|---|---|---|
| `voltage` | id сенсора | напряжение сборки, В. Необязательно, если вызываешь `process_voltage()` |
| `current` | id сенсора | А, **+ разряд, − заряд** |
| `power` | id сенсора | Вт, тот же знак. Вместе с `current`: уходит в `power_out` как есть; без `current`: I = P / V |

**Батарея** — значения с пометкой *(n)* принимают число или id `number`

| Ключ | По умолчанию | |
|---|---|---|
| `chemistry` | `lipo` | `lipo` (3.30–4.14 В) · `liion` (2.90–4.20 В) · `lifepo4` (2.50–3.45 В) · `custom` |
| `curve` | — | только для `custom`: список `{voltage: 3.7V, percent: 50}`, на одну банку |
| `cells_series` *(n)* | 1 | банок последовательно — напряжение сборки делится на это число |
| `cells_parallel` *(n)* | 1 | банок параллельно — ёмкость умножается на это число |
| `capacity` *(n)* | — | мА·ч **на одну банку** (просто число, всегда мА·ч). Если твой number уже хранит всю сборку — `cells_parallel: 1` |
| `internal_resistance` *(n)* | 0 | всей сборки, Ом — `OCV = V + I·R` |
| `restore` | `none` | `rtc`: история и уровни переживают deep sleep (ESP32, до 2 экземпляров) |
| `low` *(n)* | 10 | %, снимается на 5 % выше |
| `critical` / `resume` *(n)* | по химии | В на банку, в покое. lipo 3.50 / 3.65 · liion 3.20 / 3.40 · lifepo4 2.90 / 3.10 |

**Тонкая настройка «только напряжение»:** `smoothing` (0.15, 1 = выкл.), `only_falls` (true),
`jump_up` (0.10 В на банку — такой подъём считается зарядкой и принимается сразу),
`report_average` (10), `report_step` (0.03 В).

**Зарядка без датчика тока** (только напряжение): `charge_detect: true`. Зарядка начинается, когда два
замера подряд не ниже `charge_voltage` (lipo / liion 4.165 В на банку, lifepo4 3.50 В) или выше
сглаженного напряжения покоя на `charge_jump` (0.04 В на банку); скачок, который за 30 мин не растёт
дальше (отскок после нагрузки), отбрасывается. Заканчивается, когда два замера подряд ниже
`charge_voltage` и на 0.05 В ниже пика на зарядке. Скорость разряда — по падению процента с последней
зарядки (не меньше 2 % за 12 ч), из неё `time_left`. Нужны часы, идущие во сне (на ESP32 RTC — есть).

**Тонкая настройка с током (А):** `charge_enter_current` (−0.008), `charge_exit_current` (−0.004),
`discharge_current` (0.008), `debounce` (2 замера), `full_voltage` (4.15 В на банку, по химии),
`full_current` (−0.05: выше этого при полном напряжении = 100 %), `hold_after_charge` (20min),
`update_interval` (2s). Зарядка строго по знаку: `charge_enter_current: 0`,
`charge_exit_current: 0`, `debounce: 1`.

**Выходы** (все необязательны): `percent`, `rest_voltage` (сглаженное / с поправкой I·R),
`report_voltage` (среднее последних замеров, меняется шагами 0.03 В — удобно для BLE-пакетов),
`remaining` (мА·ч), `time_left` (ч, при разряде), `power_out` (Вт), `charging`, `low_alarm`,
`critical_alarm` (бинарные), `status` (текст).

**В лямбдах:** `id(bat).percent()`, `is_low()`, `is_critical()`, `is_charging()`, `discharge_rate_pct_h()`, `time_left_h()`, `rest_voltage()`,
`report_voltage()`, `remaining_mah()`, `voltage_for_percent(30, ток)` (напряжение сборки для процента, с током — то, что покажет
вольтметр под этой нагрузкой; например порог отключения модема), `process_voltage(v)`, `rescale(ratio)` (сменился коэффициент
делителя), `reset()`.

## Тесты

Ядро (`li_battery_core.h`) не зависит от ESPHome.

- `make -C tests/host` — около 3 млн проверок: сценарии с конкретными ожиданиями и прогоны на
  совпадение с кодом, который li_battery заменила в двух реальных устройствах (BLE-датчик движения на
  LiPo и солнечные весы для улья с INA226 и банками 18650);
- `make -C tests/host replay` — 6 дней настоящих данных INA226 с весов: 26 080 шагов, на каждом тот же
  результат, что у кода весов, и не дальше 0.74 % от того, что весы отправляли в Home Assistant;
- `python3 -m pytest tests/config` — правильные и ошибочные конфиги ESPHome.

CI запускает всё это и собирает примеры. Проверено и на реальном устройстве.

## История изменений

[CHANGELOG.md](CHANGELOG.md)
