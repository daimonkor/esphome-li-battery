# Як користуватися li_battery

[English](usage.md) · [Русский](usage.ru.md) · **Українська**

## 1. Hello world

Найкоротший робочий конфіг. Шаблонний сенсор стоїть замість справжньої напруги
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
    lambda: return 3.85;          # тут твій справжній сенсор

li_battery:
  voltage: battery_voltage
  chemistry: lipo
  percent:
    name: Battery
```

У лозі після прошивки:

```
[C][li_battery]: Li battery:
[C][li_battery]:   Chemistry: lipo (21 curve points), 1S1P
[C][li_battery]:   Mode: voltage only
[D][li_battery]: V 3.850 I nan -> rest 3.850 V, 55.0 %
```

## 2. Режим обирається за тим, що ти вимірюєш

| Що є | Що дати li_battery | Що отримаєш |
|---|---|---|
| АЦП + дільник | `voltage` | відсоток, low / critical, усереднена напруга |
| INA226 / INA219 (V та I) | `voltage`, `current` | + заряджання, статус, лічба кулонів, час, що лишився, потужність |
| V і потужність зі знаком | `voltage`, `power` | те саме, I = P / V |
| своя логіка напруги спокою | нічого — викликай `id(bat).process_voltage(v)` | відсоток, рівні |

**Твій сенсор уже має віддавати вольти / ампери / вати.** Коефіцієнт дільника, шунт, поправки — усе
це лишається у твоєму сенсорі (`filters: multiply: 2.03`). Струм і потужність: **плюс — розряд,
мінус — заряд**. `chemistry` лише обирає криву та пороги за замовчуванням, на режим не впливає.

## 3. Вимірюй напругу у спокої

Під навантаженням напруга нижча, відсоток стрибає. У режимі «лише напруга» найкраще брати вимір до
запуску Wi-Fi / BLE, наприклад в `on_boot` із пріоритетом 550:

```yaml
esphome:
  on_boot:
    - priority: 550                # АЦП готовий, радіо ще ні
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
  restore: rtc                     # deep sleep: історія зберігається
```

Зі струмом це робить `internal_resistance`: напруга спокою = V + I·R.

## 4. Налаштування з Home Assistant

Будь-яке числове налаштування — число або id `number`:

```yaml
number:
  - platform: template             # свій number
    id: cap
    optimistic: true
    restore_value: true
    min_value: 0
    max_value: 30000
    step: 100
    initial_value: 2900

li_battery:
  capacity: cap                    # береться з number
```

або нехай li_battery створить його сам (зберігається у флеші, в HA — налаштування пристрою):

```yaml
number:
  - platform: li_battery
    li_battery_id: bat
    type: capacity                 # capacity, internal_resistance, cells_series,
    name: Battery Capacity         # cells_parallel, low, critical, resume
    initial_value: 2900
```

## 5. У своїй логіці

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
            # 30 % за кривою у вигляді напруги — наприклад поріг для GSM
            lambda: return id(bat_voltage).state > id(bat).voltage_for_percent(30);
          then:
            - switch.turn_on: modem_power
```

Доступно: `percent()`, `is_low()`, `is_critical()`, `is_charging()`, `rest_voltage()`,
`report_voltage()`, `remaining_mah()`, `voltage_for_percent(p)`, `process_voltage(v)`,
`rescale(ratio)`, `reset()`.

## 6. Змінили коефіцієнт дільника з HA

Згладжена історія порахована зі старим коефіцієнтом. Скажи про це li_battery — і відсоток одразу
стане на нове значення, а не сповзатиме кілька вимірів:

```yaml
number:
  - platform: template
    id: vbat_factor
    # ...
    set_action:
      - lambda: |-
          if (id(vbat_factor).state > 0) id(bat).rescale(x / id(vbat_factor).state);
```

Перевірено на датчику руху: коефіцієнт 2.03 → 1.95 — на наступному ж пробудженні 100 % → 77 %,
4.16 → 3.99 В; назад 2.03 — одразу 100 %, 4.17 В.

## 7. Збірки

| Збірка | Конфіг |
|---|---|
| один LiPo 920 мА·год | `chemistry: lipo`, `capacity: 920` |
| 3 × 18650 по 2900 мА·год паралельно | `chemistry: liion`, `cells_parallel: 3`, `capacity: 2900` |
| 2S Li-ion | `chemistry: liion`, `cells_series: 2` — напруга всієї збірки |
| 4S LiFePO4 | `chemistry: lifepo4`, `cells_series: 4` |
| своя банка | `chemistry: custom`, `curve: [{voltage: 3.0V, percent: 0}, …]` |

Ємність — завжди мА·год **однієї банки**, просто числом.

## 8. Перевірка

```bash
make -C tests/host               # ядро: криві, обидва режими, збіг зі старим кодом
python3 -m pytest tests/config   # перевірка конфігів ESPHome: правильні та помилкові
make -C tests/host replay        # 6 днів справжніх даних INA226 з ваг
```

Як влаштовано всередині, з діаграмами: [how-it-works.uk.md](how-it-works.uk.md).
