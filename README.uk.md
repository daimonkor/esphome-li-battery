# esphome-li-battery

[English](README.md) · [Русский](README.ru.md) · **Українська**

Зовнішній компонент ESPHome: заряд батарей **LiPo / Li-ion (18650, 21700) / LiFePO4**.

- **Лише напруга** — дільник на АЦП, вимір у спокої (до запуску радіо). Згладжування, відсоток без
  заряджання лише падає, рівні «низький» / «критичний» переживають deep sleep.
- **Напруга + струм (або потужність)** — INA226 і подібні. Напруга спокою `V + I·R`, заряджання за
  знаком струму з гістерезисом і затримкою, лічба кулонів під час заряджання та 20 хв після нього,
  потім знову за кривою напруги.

`chemistry` лише обирає криву та пороги за замовчуванням — режим визначається тим, які сенсори ти
передав. Усі входи — твої сенсори; будь-яке налаштування — число або id `number`, тож його можна
змінювати з Home Assistant.

```yaml
external_components:
  - source: github://daimonkor/esphome-li-battery@v1.2.0
    components: [li_battery]
```

**Документація:** [як користуватися](docs/usage.uk.md) · [як влаштовано, з діаграмами](docs/how-it-works.uk.md)

## Hello world

```yaml
sensor:
  - platform: template
    id: battery_voltage
    unit_of_measurement: V
    lambda: return 3.85;          # тут твій справжній сенсор (adc, ina226, ...)

li_battery:
  voltage: battery_voltage        # вольти, дільник уже враховано
  chemistry: lipo
  percent:
    name: Battery                 # -> 55 %
```

## Лише напруга (LiPo на дільнику АЦП)

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
  capacity: 920          # мА·год
  restore: rtc           # історія переживає deep sleep
  percent:
    name: Battery
  low_alarm:
    name: Battery low
```

Можна й без вхідного сенсора — віддавай напругу спокою сам:
`id(bat).process_voltage(v)` повертає відсоток.

## Напруга + струм (INA226, 3 × 18650 паралельно)

```yaml
li_battery:
  id: bat
  voltage: bat_voltage   # В
  current: bat_current   # А, + розряд, − заряд   (або  power: Вт, той самий знак)
  chemistry: liion
  cells_parallel: 3
  capacity: 2900         # мА·год на банку -> 8700 мА·год
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
  - platform: li_battery   # або capacity: <id свого number>
    li_battery_id: bat
    type: capacity
    name: Battery Capacity
    initial_value: 2900
```

Ще приклади — у [`examples/`](examples), зокрема два реальні пристрої:
[`move_sensor_a0.yaml`](examples/move_sensor_a0.yaml) (лише A0 з дільником) і
[`hivescale_ina226.yaml`](examples/hivescale_ina226.yaml) (INA226, 3 × 18650).

## Налаштування

**Входи**

| Ключ | | |
|---|---|---|
| `voltage` | id сенсора | напруга збірки, В. Необов'язково, якщо викликаєш `process_voltage()` |
| `current` | id сенсора | А, **+ розряд, − заряд** |
| `power` | id сенсора | Вт, той самий знак. Разом із `current`: іде в `power_out` як є; без `current`: I = P / V |

**Батарея** — значення з позначкою *(n)* приймають число або id `number`

| Ключ | За замовчуванням | |
|---|---|---|
| `chemistry` | `lipo` | `lipo` (3.30–4.14 В) · `liion` (2.90–4.20 В) · `lifepo4` (2.50–3.45 В) · `custom` |
| `curve` | — | лише для `custom`: список `{voltage: 3.7V, percent: 50}`, на одну банку |
| `cells_series` *(n)* | 1 | банок послідовно — напруга збірки ділиться на це число |
| `cells_parallel` *(n)* | 1 | банок паралельно — ємність множиться на це число |
| `capacity` *(n)* | — | мА·год **на одну банку** (просто число, завжди мА·год). Якщо твій number уже зберігає всю збірку — `cells_parallel: 1` |
| `internal_resistance` *(n)* | 0 | усієї збірки, Ом — `OCV = V + I·R` |
| `restore` | `none` | `rtc`: історія та рівні переживають deep sleep (ESP32, до 2 екземплярів) |
| `low` *(n)* | 10 | %, знімається на 5 % вище |
| `critical` / `resume` *(n)* | за хімією | В на банку, у спокої. lipo 3.50 / 3.65 · liion 3.20 / 3.40 · lifepo4 2.90 / 3.10 |

**Тонке налаштування «лише напруга»:** `smoothing` (0.15, 1 = вимк.), `only_falls` (true),
`jump_up` (0.10 В на банку — такий підйом вважається заряджанням і приймається одразу),
`report_average` (10), `report_step` (0.03 В).

**Заряджання без датчика струму** (лише напруга): `charge_detect: true`. Заряджання починається, коли
два виміри поспіль не нижчі за `charge_voltage` (lipo / liion 4.165 В на банку, lifepo4 3.50 В) або
вищі за згладжену напругу спокою на `charge_jump` (0.04 В на банку); стрибок, що за 30 хв не росте
далі (відскок після навантаження), відкидається. Закінчується, коли два виміри поспіль нижчі за
`charge_voltage` і на 0.05 В нижчі за пік на зарядці. Швидкість розряду — за падінням відсотка з
останнього заряджання (не менше 2 % за 12 год), з неї `time_left`. Потрібен годинник, що йде уві сні (на ESP32 RTC — є).

**Тонке налаштування зі струмом (А):** `charge_enter_current` (−0.008), `charge_exit_current` (−0.004),
`discharge_current` (0.008), `debounce` (2 виміри), `full_voltage` (4.15 В на банку, за хімією),
`full_current` (−0.05: вище цього за повної напруги = 100 %), `hold_after_charge` (20min),
`update_interval` (2s). Заряджання суворо за знаком: `charge_enter_current: 0`,
`charge_exit_current: 0`, `debounce: 1`.

**Виходи** (усі необов'язкові): `percent`, `rest_voltage` (згладжена / з поправкою I·R),
`report_voltage` (середнє останніх вимірів, змінюється кроками 0.03 В — зручно для BLE-пакетів),
`remaining` (мА·год), `time_left` (год, під час розряду), `power_out` (Вт), `charging`, `low_alarm`,
`critical_alarm` (бінарні), `status` (текст).

**У лямбдах:** `id(bat).percent()`, `is_low()`, `is_critical()`, `is_charging()`, `discharge_rate_pct_h()`, `time_left_h()`, `rest_voltage()`,
`report_voltage()`, `remaining_mah()`, `voltage_for_percent(30, струм)` (напруга збірки для відсотка, зі струмом — те, що покаже
вольтметр під цим навантаженням; наприклад поріг вимкнення модема), `process_voltage(v)`, `rescale(ratio)` (змінився коефіцієнт
дільника), `reset()`.

## Тести

Ядро (`li_battery_core.h`) не залежить від ESPHome.

- `make -C tests/host` — близько 3 млн перевірок: сценарії з конкретними очікуваннями та прогони на
  збіг із кодом, який li_battery замінила у двох реальних пристроях (BLE-датчик руху на LiPo та сонячні
  ваги для вулика з INA226 і банками 18650);
- `make -C tests/host replay` — 6 днів справжніх даних INA226 з ваг: 26 080 кроків, на кожному той самий
  результат, що в коду ваг, і не далі 0.74 % від того, що ваги надсилали в Home Assistant;
- `python3 -m pytest tests/config` — правильні та помилкові конфіги ESPHome.

CI запускає все це й збирає приклади. Перевірено й на реальному пристрої.

## Історія змін

[CHANGELOG.md](CHANGELOG.md)
