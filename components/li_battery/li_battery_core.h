#pragma once
// li_battery — state of charge for lithium batteries (LiPo / Li-ion / LiFePO4).
// Pure C++, no ESPHome dependency: the same code runs in firmware and in host tests.
//
// Two modes, picked by whether a current reading is available:
//  * voltage only  — rest voltage measured with little load (e.g. ADC divider before the radio
//                    starts). Smoothing, "percent only falls unless charging", low / critical.
//                    Optional (charge_detect): charging guessed from the voltage alone — a jump
//                    above the smoothed rest voltage or a near-full voltage, until it falls again —
//                    and the discharge rate since the last charge (time left without a current sensor).
//  * voltage + current (e.g. INA226) — open-circuit voltage = V + I·R (I > 0 discharge, I < 0
//                    charge), charging state with hysteresis + debounce, coulomb counting while
//                    charging and for a hold time after it, then back to the voltage curve.
//
// Conventions: voltages are for the whole pack (cells_series cells in series); the curve is per
// cell. Capacity is in mAh per cell, multiplied by cells_parallel. Current in amperes.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace li_battery_core {

struct CurvePoint {
  float v;    // rest (open-circuit) voltage of one cell, V
  float pct;  // state of charge, %
};

enum class Chemistry : uint8_t { LIPO = 0, LIION = 1, LIFEPO4 = 2, CUSTOM = 3 };

// LiPo pouch, rest voltage at low current. Top: 4.14 V rest = 100 % (after charge ends at 4.20 V
// the cell settles to ~4.15-4.17 V within an hour or two — still full).
static const CurvePoint CURVE_LIPO[] = {
    {3.30f, 0},  {3.50f, 2},  {3.61f, 5},  {3.69f, 10}, {3.71f, 15}, {3.73f, 20}, {3.75f, 25},
    {3.77f, 30}, {3.79f, 35}, {3.80f, 40}, {3.82f, 45}, {3.84f, 50}, {3.85f, 55}, {3.87f, 60},
    {3.91f, 65}, {3.95f, 70}, {3.98f, 75}, {4.02f, 80}, {4.06f, 87}, {4.10f, 94}, {4.14f, 100}};

// Li-ion 18650 / 21700 (NMC, LCO), open-circuit voltage.
static const CurvePoint CURVE_LIION[] = {
    {2.90f, 0},  {3.20f, 1},  {3.40f, 5},  {3.60f, 15}, {3.70f, 25}, {3.80f, 40}, {3.85f, 50},
    {3.90f, 60}, {3.95f, 70}, {4.00f, 80}, {4.05f, 88}, {4.10f, 94}, {4.15f, 98}, {4.20f, 100}};

// LiFePO4, open-circuit voltage (very flat middle — voltage-only estimates are rough there).
static const CurvePoint CURVE_LIFEPO4[] = {
    {2.50f, 0},  {2.90f, 5},  {3.10f, 10}, {3.20f, 20}, {3.22f, 30}, {3.25f, 40}, {3.26f, 50},
    {3.27f, 60}, {3.28f, 70}, {3.30f, 80}, {3.32f, 90}, {3.35f, 95}, {3.40f, 99}, {3.45f, 100}};

struct Curve {
  const CurvePoint *p = nullptr;
  size_t n = 0;
};

inline Curve builtin_curve(Chemistry c) {
  switch (c) {
    case Chemistry::LIION:
      return {CURVE_LIION, sizeof(CURVE_LIION) / sizeof(CURVE_LIION[0])};
    case Chemistry::LIFEPO4:
      return {CURVE_LIFEPO4, sizeof(CURVE_LIFEPO4) / sizeof(CURVE_LIFEPO4[0])};
    case Chemistry::LIPO:
    default:
      return {CURVE_LIPO, sizeof(CURVE_LIPO) / sizeof(CURVE_LIPO[0])};
  }
}

// Cell voltage -> %, linear between points, clamped to the first / last point.
inline float pct_from_voltage(const Curve &c, float v) {
  if (c.n == 0 || std::isnan(v)) return NAN;
  if (v <= c.p[0].v) return c.p[0].pct;
  if (v >= c.p[c.n - 1].v) return c.p[c.n - 1].pct;
  for (size_t i = 1; i < c.n; i++) {
    if (v < c.p[i].v) {
      const float t = (v - c.p[i - 1].v) / (c.p[i].v - c.p[i - 1].v);
      return c.p[i - 1].pct + t * (c.p[i].pct - c.p[i - 1].pct);
    }
  }
  return c.p[c.n - 1].pct;
}

// % -> cell voltage (inverse of the curve), e.g. to turn "stop the modem below 30 %" into a voltage.
inline float voltage_from_pct(const Curve &c, float pct) {
  if (c.n == 0 || std::isnan(pct)) return NAN;
  if (pct <= c.p[0].pct) return c.p[0].v;
  if (pct >= c.p[c.n - 1].pct) return c.p[c.n - 1].v;
  for (size_t i = 1; i < c.n; i++) {
    if (pct <= c.p[i].pct) {
      const float span = c.p[i].pct - c.p[i - 1].pct;
      if (span <= 0) return c.p[i].v;
      const float t = (pct - c.p[i - 1].pct) / span;
      return c.p[i - 1].v + t * (c.p[i].v - c.p[i - 1].v);
    }
  }
  return c.p[c.n - 1].v;
}

struct Config {
  Curve curve = builtin_curve(Chemistry::LIPO);
  uint8_t cells_series = 1;
  uint8_t cells_parallel = 1;
  float capacity_mah = 0;          // per cell; 0 = unknown (no coulomb counting, no remaining)
  float internal_resistance = 0;   // whole pack, ohm

  // voltage-only mode
  float smoothing = 0.15f;         // EMA factor for the rest voltage, 1 = off
  float jump_up_v = 0.10f;         // per cell: a rise bigger than this is accepted at once (charger)
  bool only_falls = true;          // without a jump the percent may only fall ...
  float rise_accept_pct = 10;      // ... unless it rises by this much at once
  bool round_percent = true;       // whole percents (also used for only_falls)

  // levels (both modes)
  float low_pct = 10, low_clear_pct = 15;
  float critical_v = NAN, resume_v = NAN;   // per cell rest voltage; NAN = off

  // reported voltage (voltage-only): average of the last N, new value only on a change >= step
  uint8_t report_avg_n = 10;
  float report_step_v = 0.03f;
  float report_reset_v = 0.15f;    // pack: a jump bigger than this restarts the average

  // voltage only: charging from the voltage alone (no charge-status pin, no current sensor).
  // Real case (Move Sensor BLE, XIAO ESP32-C3 + LiPo): on the charger 4.17-4.21 V, right after
  // unplugging it rests at 4.15 V for hours, a part-charged cell on the charger reads 4.10-4.15 V.
  bool charge_detect = false;
  float charge_v = 4.165f;         // per cell: at or above this -> on the charger
  float charge_jump_v = 0.04f;     // per cell: this much above the smoothed rest voltage -> plugged in
  float charge_drop_v = 0.05f;     // per cell: below charge_v - charge_hyst_v and this much under the peak ...
  float charge_hyst_v = 0.01f;     //   (a reading at the charger's taper end may dip just under charge_v)
  uint8_t charge_drop_n = 2;       // ... this many readings in a row -> unplugged
  float charge_rise_v = 0.02f;     // after the start it must climb this much more (or reach charge_v) ...
  uint32_t charge_confirm_ms = 30UL * 60UL * 1000UL;   // ... within this time, else it was a rebound
  uint32_t charge_stale_ms = 2UL * 3600UL * 1000UL;    // no reading this long -> charging state dropped
  // discharge rate (voltage only, charge_detect): from the percent drop since the last charge
  float rate_min_drop_pct = 2;     // need at least this drop ...
  uint32_t rate_min_ms = 12UL * 3600UL * 1000UL;       // ... over at least this time

  // voltage + current mode
  float charge_enter_a = -0.008f;  // I below this -> charging
  float charge_exit_a = -0.004f;   // charging until I rises above this
  float discharge_a = 0.008f;      // I above this -> discharging (status only)
  uint8_t debounce = 2;            // updates in a row before the charging state flips
  float full_v = 4.15f;            // per cell: at or above this ...
  float full_a = -0.05f;           // ... with I above this (charge current tapered off) -> 100 %
  uint32_t hold_after_charge_ms = 20UL * 60UL * 1000UL;
  float max_dt_s = 10;             // longer gaps (sleep) are not integrated
};

enum class Status : uint8_t { UNKNOWN = 0, CHARGING, DISCHARGING, IDLE };

inline const char *status_str(Status s) {
  switch (s) {
    case Status::CHARGING: return "Charging";
    case Status::DISCHARGING: return "Discharging";
    case Status::IDLE: return "Idle";
    default: return "No data";
  }
}

// Everything that has to survive deep sleep (kept in RTC memory with `restore: rtc`).
// 1.2.0: "LiB2" — the struct grew (charge detection), an old RTC copy after an OTA is dropped
static constexpr uint32_t STATE_MAGIC = 0x4C694232u;  // "LiB2"
static constexpr uint8_t REPORT_MAX_N = 16;

struct State {
  uint32_t magic = STATE_MAGIC;
  // voltage-only
  float vf = 0;                    // smoothed rest voltage per cell, 0 = no history
  int16_t pct_last = -1;           // last whole percent, -1 = none
  float ring[REPORT_MAX_N] = {0};
  uint8_t ring_n = 0, ring_i = 0;
  float report_v = 0;              // last reported pack voltage, 0 = none
  // levels
  bool low = false, critical = false;
  // voltage + current
  bool charging = false;
  uint8_t debounce_cnt = 0;
  float soc_cc = -1;               // coulomb-counted %, -1 = not counting
  float last_idle_soc = -1;        // last % from the curve at rest, -1 = unknown
  int64_t charge_end_ms = 0;
  int64_t last_ms = 0;             // time of the previous update, 0 = none
  // voltage-only charge detection (charging / debounce_cnt above are shared with the current mode)
  int64_t chg_since_ms = 0;
  float chg_start_v = 0;           // per cell, first reading on the charger
  float chg_peak_v = 0;            // per cell, highest reading on the charger
  uint8_t chg_drops = 0;
  bool chg_confirmed = false;
  // discharge rate: percent and time at the anchor (charge end or first reading)
  float rate_pct0 = -1;
  int64_t rate_ms0 = 0;
  float rate_pct_h = NAN;          // %/h, > 0 = discharging; NAN = not known yet
  // last results
  float percent = NAN;
  float ocv = NAN;                 // pack, after smoothing / I·R compensation
  Status status = Status::UNKNOWN;

  void reset() { *this = State(); }
  bool valid() const { return magic == STATE_MAGIC; }
};

class Estimator {
 public:
  Config cfg;
  State *st = nullptr;

  Estimator() = default;
  Estimator(const Config &c, State *s) : cfg(c), st(s) {}

  float capacity_total_mah() const { return cfg.capacity_mah * (cfg.cells_parallel ? cfg.cells_parallel : 1); }
  uint8_t series() const { return cfg.cells_series ? cfg.cells_series : 1; }

  // Voltage-only update: v_pack = rest voltage of the pack. Returns the percent.
  // now_ms: a clock that keeps running through deep sleep (needed for charge_detect timing and the
  // discharge rate; 0 = no clock: the time-based rules are skipped).
  float update_voltage(float v_pack, int64_t now_ms = 0) {
    if (std::isnan(v_pack)) return st->percent;
    report_filter_(v_pack);
    const float v = v_pack / series();
    bool charge_ended = false;
    if (cfg.charge_detect) charge_ended = charge_voltage_(v, now_ms);
    const bool jump = st->vf > 0 && v - st->vf > cfg.jump_up_v;
    // on the charger the reading follows the charger at once; just unplugged -> start over from it
    if (st->vf <= 0 || jump || st->charging || charge_ended) st->vf = v;
    else st->vf += cfg.smoothing * (v - st->vf);
    float p = pct_from_voltage(cfg.curve, st->vf);
    if (cfg.round_percent) p = roundf(p);
    if (cfg.only_falls && st->pct_last >= 0 && p > st->pct_last && !jump && !st->charging && !charge_ended &&
        p < st->pct_last + cfg.rise_accept_pct)
      p = st->pct_last;
    st->pct_last = (int16_t) lroundf(p);
    st->ocv = st->vf * series();
    st->status = cfg.charge_detect ? (st->charging ? Status::CHARGING : Status::DISCHARGING) : Status::UNKNOWN;
    levels_(p, st->vf);
    st->percent = p;
    if (cfg.charge_detect) rate_(p, now_ms, charge_ended);
    return p;
  }

  // %/h of discharge since the last charge (voltage only, charge_detect), NAN while unknown
  float discharge_rate_pct_h() const { return st->rate_pct_h; }

  // Voltage + current update (call at a steady rate, e.g. every 2 s). i > 0 discharge, i < 0 charge.
  float update(float v_pack, float i, int64_t now_ms) {
    if (std::isnan(i)) return update_voltage(v_pack);
    if (std::isnan(v_pack)) return st->percent;
    float dt = 0;
    if (st->last_ms > 0 && now_ms > st->last_ms) dt = (float) (now_ms - st->last_ms) / 1000.0f;
    if (dt > cfg.max_dt_s) dt = 0;
    st->last_ms = now_ms;

    const float ocv_pack = v_pack + i * cfg.internal_resistance;
    const float ocv_cell = ocv_pack / series();
    const float v_cell = v_pack / series();
    const float soc_ocv = pct_from_voltage(cfg.curve, ocv_cell);
    const float cap = capacity_total_mah();
    const bool cap_valid = !std::isnan(cap) && cap > 100.0f;

    const bool candidate = st->charging ? (i < cfg.charge_exit_a) : (i < cfg.charge_enter_a);
    bool charging = st->charging;
    if (candidate == st->charging) {
      st->debounce_cnt = 0;
    } else if (++st->debounce_cnt >= cfg.debounce) {
      st->debounce_cnt = 0;
      charging = candidate;
    }

    if (charging) {
      if (!st->charging) {
        // charge start: keep a running count (short dip), else start from the last rest value
        if (st->soc_cc >= 0) {
        } else if (st->last_idle_soc >= 0) {
          st->soc_cc = st->last_idle_soc;
        } else {
          st->soc_cc = soc_ocv;
        }
        st->charging = true;
      } else if (cap_valid && st->soc_cc >= 0) {
        if (v_cell >= cfg.full_v && i > cfg.full_a) {
          st->soc_cc = 100.0f;
        } else {
          const float dq = (-i * 1000.0f) * dt / 3600.0f;
          st->soc_cc = fminf(100.0f, st->soc_cc + dq / cap * 100.0f);
        }
      }
    } else {
      if (st->charging) st->charge_end_ms = now_ms;
      st->charging = false;
      if (st->soc_cc >= 0) {
        if (now_ms - st->charge_end_ms < (int64_t) cfg.hold_after_charge_ms) {
          if (cap_valid) {
            const float dq = (-i * 1000.0f) * dt / 3600.0f;
            st->soc_cc = fmaxf(0.0f, fminf(100.0f, st->soc_cc + dq / cap * 100.0f));
          }
        } else {
          st->soc_cc = -1.0f;   // hold expired — back to the voltage curve
        }
      } else if (!candidate) {
        st->last_idle_soc = soc_ocv;
      }
    }

    const float p = st->soc_cc >= 0 ? st->soc_cc : soc_ocv;
    st->ocv = ocv_pack;
    st->status = st->charging ? Status::CHARGING : (i > cfg.discharge_a ? Status::DISCHARGING : Status::IDLE);
    levels_(p, ocv_cell);
    st->percent = p;
    return p;
  }

  // Voltage scale changed (e.g. a divider factor was edited): rescale the history instead of
  // treating the new reading as a jump.
  void rescale(float ratio) {
    if (!(ratio > 0) || std::isnan(ratio)) return;
    if (st->vf > 0) st->vf *= ratio;
  }

  float remaining_mah() const {
    const float cap = capacity_total_mah();
    if (!(cap > 0) || std::isnan(st->percent)) return NAN;
    return cap * st->percent / 100.0f;
  }

  // hours left at the given discharge current (A); NAN when not discharging.
  // Without a current (NAN) in voltage-only mode with charge_detect: from the discharge rate.
  float time_left_h(float i) const {
    if (std::isnan(i)) {
      if (st->charging || std::isnan(st->percent) || !(st->rate_pct_h > 0)) return NAN;
      return st->percent / st->rate_pct_h;
    }
    const float rem = remaining_mah();
    if (std::isnan(rem) || i <= cfg.discharge_a) return NAN;
    return rem / (i * 1000.0f);
  }

  float report_voltage() const { return st->report_v > 0 ? st->report_v : NAN; }

  // Signed power (W, > 0 discharge) -> current (A) when there is a power sensor but no current sensor.
  static float current_from_power(float p_w, float v_pack) {
    if (std::isnan(p_w) || std::isnan(v_pack) || v_pack < 0.5f) return NAN;
    return p_w / v_pack;
  }
  // Pack power, W (> 0 discharge, < 0 charge)
  static float power(float v_pack, float i) {
    if (std::isnan(v_pack) || std::isnan(i)) return NAN;
    return v_pack * i;
  }

 protected:
  void levels_(float p, float v_cell) {
    if (!std::isnan(p)) {
      if (p <= cfg.low_pct) st->low = true;
      else if (p >= cfg.low_clear_pct) st->low = false;
    }
    if (!std::isnan(cfg.critical_v) && v_cell > 0) {
      if (v_cell <= cfg.critical_v) st->critical = true;
      else if (!std::isnan(cfg.resume_v) && v_cell >= cfg.resume_v) st->critical = false;
      else if (std::isnan(cfg.resume_v) && v_cell > cfg.critical_v) st->critical = false;
    }
  }

  // Charging from the voltage alone. v = cell reading, compared with the smoothed rest voltage
  // before it is updated. Returns true on the reading where charging ended.
  bool charge_voltage_(float v, int64_t now_ms) {
    const bool clock = now_ms > 0;
    if (clock && st->last_ms > 0 && now_ms - st->last_ms > (int64_t) cfg.charge_stale_ms && st->charging) {
      st->charging = false;   // nothing heard for hours: do not keep "charging" from back then
      st->debounce_cnt = 0;
    }
    if (clock) st->last_ms = now_ms;
    if (!st->charging) {
      const bool cand = v >= cfg.charge_v || (st->vf > 0 && v - st->vf >= cfg.charge_jump_v);
      if (!cand) {
        st->debounce_cnt = 0;
        return false;
      }
      if (++st->debounce_cnt < (cfg.debounce ? cfg.debounce : 1)) return false;
      st->debounce_cnt = 0;
      st->charging = true;
      st->chg_since_ms = now_ms;
      st->chg_start_v = v;   // a rebound after a load jumps once and stays; a charger keeps climbing
      st->chg_peak_v = v;
      st->chg_drops = 0;
      st->chg_confirmed = v >= cfg.charge_v;
      return false;
    }
    if (v > st->chg_peak_v) st->chg_peak_v = v;
    if (v >= cfg.charge_v || st->chg_peak_v >= st->chg_start_v + cfg.charge_rise_v - 1e-4f)
      st->chg_confirmed = true;
    if (v < cfg.charge_v - cfg.charge_hyst_v && v <= st->chg_peak_v - cfg.charge_drop_v + 1e-4f) st->chg_drops++;
    else st->chg_drops = 0;
    const bool unplugged = st->chg_drops >= (cfg.charge_drop_n ? cfg.charge_drop_n : 1);
    const bool rebound = !st->chg_confirmed && clock && st->chg_since_ms > 0 &&
                         now_ms - st->chg_since_ms >= (int64_t) cfg.charge_confirm_ms;
    if (!unplugged && !rebound) return false;
    st->charging = false;
    st->chg_drops = 0;
    return unplugged;   // a rebound changes nothing in the history
  }

  // Discharge rate since the anchor (charge end / first reading / a rise of the percent).
  void rate_(float p, int64_t now_ms, bool charge_ended) {
    if (now_ms <= 0 || std::isnan(p)) return;
    if (st->charging) {
      st->rate_pct0 = -1;   // new anchor after the charge
      return;
    }
    if (charge_ended || st->rate_pct0 < 0 || p > st->rate_pct0 || now_ms < st->rate_ms0) {
      // the old rate stays until the new anchor has enough data
      st->rate_pct0 = p;
      st->rate_ms0 = now_ms;
      return;
    }
    const float drop = st->rate_pct0 - p;
    const int64_t dt = now_ms - st->rate_ms0;
    if (drop >= cfg.rate_min_drop_pct && dt >= (int64_t) cfg.rate_min_ms)
      st->rate_pct_h = drop / ((float) dt / 3600000.0f);
  }

  void report_filter_(float v) {
    if (v <= 0.5f) return;
    uint8_t n_max = cfg.report_avg_n;
    if (n_max < 1) n_max = 1;
    if (n_max > REPORT_MAX_N) n_max = REPORT_MAX_N;
    if (st->report_v > 0 && fabsf(v - st->report_v) > cfg.report_reset_v) {
      st->ring_n = 0;
      st->ring_i = 0;
    }
    st->ring[st->ring_i] = v;
    st->ring_i = (uint8_t) ((st->ring_i + 1) % n_max);
    if (st->ring_n < n_max) st->ring_n++;
    float sum = 0;
    for (uint8_t k = 0; k < st->ring_n; k++) sum += st->ring[k];
    const float avg = sum / st->ring_n;
    if (st->report_v <= 0 || fabsf(avg - st->report_v) >= cfg.report_step_v) st->report_v = roundf(avg * 100.0f) / 100.0f;
  }
};

}  // namespace li_battery_core
