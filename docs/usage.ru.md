# Как пользоваться li_battery

[English](usage.md) · **Русский** · [Українська](usage.uk.md)

## 1. Hello world

Самый короткий рабочий конфиг. Шаблонный сенсор стоит вместо настоящего напряжения
([`examples/hello_world.yaml`](../examples/hello_world.yaml)):

```yaml
external_components:
  - source: github://daimonkor/esphome-li-battery@v1.2.1
    components: [li_battery]

sensor:
  - platform: template
    id: battery_voltage
    unit_of_measurement: V
    update_interval: 10s
    lambda: return 3.85;          # здесь твой настоящий сенсор

li_battery:
  voltage: battery_voltage
  chemistry: lipo
  percent:
    name: Battery
```

В логе после прошивки:

```
[C][li_battery]: Li battery:
[C][li_battery]:   Chemistry: lipo (21 curve points), 1S1P
[C][li_battery]:   Mode: voltage only
[D][li_battery]: V 3.850 I nan -> rest 3.850 V, 55.0 %
```

## 2. Режим выбирается по тому, что ты меряешь

| Что есть | Что дать li_battery | Что получишь |
|---|---|---|
| АЦП + делитель | `voltage` | процент, low / critical, усреднённое напряжение (+ зарядка и время работы с `charge_detect: true`) |
| INA226 / INA219 (V и I) | `voltage`, `current` | + зарядка, статус, счёт кулонов, оставшееся время, мощность |
| V и мощность со знаком | `voltage`, `power` | то же, I = P / V |
| своя логика напряжения покоя | ничего — вызывай `id(bat).process_voltage(v)` | процент, уровни |

**Твой сенсор уже должен отдавать вольты / амперы / ватты.** Коэффициент делителя, шунт, поправки —
всё это остаётся в твоём сенсоре (`filters: multiply: 2.03`). Ток и мощность: **плюс — разряд, минус — заряд**.
`chemistry` только выбирает кривую и пороги по умолчанию, на режим не влияет.

## 3. Меряй напряжение в покое

Под нагрузкой напряжение ниже, процент прыгает. В режиме «только напряжение» лучше всего брать замер
до запуска Wi-Fi / BLE, например в `on_boot` с приоритетом 550:

```yaml
esphome:
  on_boot:
    - priority: 550                # АЦП готов, радио ещё нет
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
  restore: rtc                     # deep sleep: история сохраняется
```

С током это делает `internal_resistance`: напряжение покоя = V + I·R.

## 4. Настройки из Home Assistant

Любая числовая настройка — число или id `number`:

```yaml
number:
  - platform: template             # свой number
    id: cap
    optimistic: true
    restore_value: true
    min_value: 0
    max_value: 30000
    step: 100
    initial_value: 2900

li_battery:
  capacity: cap                    # берётся из number
```

или пусть li_battery создаст его сам (хранится во флеше, в HA — настройка устройства):

```yaml
number:
  - platform: li_battery
    li_battery_id: bat
    type: capacity                 # capacity, internal_resistance, cells_series,
    name: Battery Capacity         # cells_parallel, low, critical, resume
    initial_value: 2900
```

## 5. В своей логике

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
            # 30 % по кривой в виде напряжения — например порог для GSM
            lambda: return id(bat_voltage).state > id(bat).voltage_for_percent(30);
          then:
            - switch.turn_on: modem_power
```

Доступно: `percent()`, `is_low()`, `is_critical()`, `is_charging()`, `discharge_rate_pct_h()`, `time_left_h()`, `rest_voltage()`,
`report_voltage()`, `remaining_mah()`, `voltage_for_percent(p, ток)`, `process_voltage(v)`,
`rescale(ratio)`, `reset()`.

## 6. Поменяли коэффициент делителя из HA

Сглаженная история посчитана со старым коэффициентом. Скажи об этом li_battery — и процент сразу
встанет на новое значение, а не будет сползать несколько замеров:

```yaml
number:
  - platform: template
    id: vbat_factor
    # ...
    set_action:
      - lambda: |-
          if (id(vbat_factor).state > 0) id(bat).rescale(x / id(vbat_factor).state);
```

Проверено на датчике движения: коэффициент 2.03 → 1.95 — на следующем же пробуждении 100 % → 77 %,
4.16 → 3.99 В; обратно 2.03 — сразу 100 %, 4.17 В.

## 7. Сборки

| Сборка | Конфиг |
|---|---|
| один LiPo 920 мА·ч | `chemistry: lipo`, `capacity: 920` |
| 3 × 18650 по 2900 мА·ч параллельно | `chemistry: liion`, `cells_parallel: 3`, `capacity: 2900` |
| 2S Li-ion | `chemistry: liion`, `cells_series: 2` — напряжение всей сборки |
| 4S LiFePO4 | `chemistry: lifepo4`, `cells_series: 4` |
| своя банка | `chemistry: custom`, `curve: [{voltage: 3.0V, percent: 0}, …]` |

Ёмкость — всегда мА·ч **одной банки**, просто числом.

## 8. Проверка

```bash
make -C tests/host               # ядро: кривые, оба режима, совпадение со старым кодом
python3 -m pytest tests/config   # проверка конфигов ESPHome: правильные и ошибочные
make -C tests/host replay        # 6 дней настоящих данных INA226 с весов
```

Как устроено внутри, с диаграммами: [how-it-works.ru.md](how-it-works.ru.md).
