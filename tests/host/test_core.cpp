// Host tests for li_battery_core.h:  make -C tests/host
// The "legacy" functions below are copies of the code li_battery replaces — the Move Sensor BLE
// firmware (voltage only, LiPo) and the HiveScale scales (INA226 voltage + current, Li-ion) —
// and the library must give the same results on the same inputs.
#include "../../components/li_battery/li_battery_core.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <string>
#include <algorithm>

using namespace li_battery_core;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    g_checks++;                                                          \
    if (!(c)) {                                                          \
      g_fail++;                                                          \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
    }                                                                    \
  } while (0)
#define CHECK_NEAR(a, b, eps)                                                                 \
  do {                                                                                        \
    g_checks++;                                                                               \
    const double _a = (a), _b = (b);                                                          \
    if (!(std::fabs(_a - _b) <= (eps) || (std::isnan(_a) && std::isnan(_b)))) {               \
      g_fail++;                                                                               \
      std::printf("FAIL %s:%d  %s = %.6f, %s = %.6f\n", __FILE__, __LINE__, #a, _a, #b, _b);  \
    }                                                                                         \
  } while (0)

// ------------------------------------------------------------------ legacy: Move Sensor BLE
namespace legacy_ms {
static float bat_vf = 0;
static int8_t bat_pct = -1;
static bool bat_low = false, bat_crit = false;
static constexpr float BAT_CRIT_V = 3.50f, BAT_RESUME_V = 3.65f;
static constexpr uint8_t VBAT_AVG_N = 10;
static constexpr float VBAT_STEP = 0.03f;
static float vbat_ring[VBAT_AVG_N] = {0};
static uint8_t vbat_ring_n = 0, vbat_ring_i = 0;
static float vbat_sent = 0;
static void reset() {
  bat_vf = 0; bat_pct = -1; bat_low = bat_crit = false;
  for (auto &x : vbat_ring) x = 0;
  vbat_ring_n = vbat_ring_i = 0; vbat_sent = 0;
}
static inline float vbat_filter(float v) {
  if (std::isnan(v) || v <= 0.5f) return vbat_sent > 0 ? vbat_sent : v;
  if (vbat_sent > 0 && fabsf(v - vbat_sent) > 0.15f) { vbat_ring_n = 0; vbat_ring_i = 0; }
  vbat_ring[vbat_ring_i] = v;
  vbat_ring_i = (uint8_t) ((vbat_ring_i + 1) % VBAT_AVG_N);
  if (vbat_ring_n < VBAT_AVG_N) vbat_ring_n++;
  float sum = 0;
  for (uint8_t i = 0; i < vbat_ring_n; i++) sum += vbat_ring[i];
  const float avg = sum / vbat_ring_n;
  if (vbat_sent <= 0 || fabsf(avg - vbat_sent) >= VBAT_STEP) vbat_sent = roundf(avg * 100.0f) / 100.0f;
  return vbat_sent;
}
static inline float lipo_pct(float v) {
  static const float T[][2] = {
    {3.30f, 0},  {3.50f, 2},  {3.61f, 5},  {3.69f, 10}, {3.71f, 15}, {3.73f, 20},
    {3.75f, 25}, {3.77f, 30}, {3.79f, 35}, {3.80f, 40}, {3.82f, 45}, {3.84f, 50},
    {3.85f, 55}, {3.87f, 60}, {3.91f, 65}, {3.95f, 70}, {3.98f, 75}, {4.02f, 80},
    {4.06f, 87}, {4.10f, 94}, {4.14f, 100}};
  const int n = sizeof(T) / sizeof(T[0]);
  if (v <= T[0][0]) return 0;
  if (v >= T[n - 1][0]) return 100;
  for (int i = 1; i < n; i++)
    if (v < T[i][0])
      return T[i - 1][1] + (v - T[i - 1][0]) * (T[i][1] - T[i - 1][1]) / (T[i][0] - T[i - 1][0]);
  return 100;
}
// on_value body of the "Voltage" ADC sensor (rest voltage already in v)
static int step(float v, float *tx, float *rest) {
  *tx = vbat_filter(v);
  const bool charge_jump = bat_vf > 0 && v - bat_vf > 0.10f;
  if (bat_vf <= 0 || charge_jump) bat_vf = v;
  else bat_vf += 0.15f * (v - bat_vf);
  int p = (int) roundf(lipo_pct(bat_vf));
  if (bat_pct >= 0 && p > bat_pct && !charge_jump && p < bat_pct + 10) p = bat_pct;
  bat_pct = p;
  if (p <= 10) bat_low = true;
  else if (p >= 15) bat_low = false;
  if (bat_vf > 0 && bat_vf <= BAT_CRIT_V) bat_crit = true;
  else if (bat_vf >= BAT_RESUME_V) bat_crit = false;
  *rest = bat_vf;
  return p;
}
}  // namespace legacy_ms

// ------------------------------------------------------------------ legacy: HiveScale scales
namespace legacy_hs {
constexpr float BAT_R_INT_OHM = 0.14f;
constexpr uint32_t SOC_HOLD_AFTER_CHARGE_MS = 20UL * 60UL * 1000UL;
constexpr float BAT_CHARGE_ENTER_A = -0.008f, BAT_CHARGE_EXIT_A = -0.004f, BAT_DISCHARGE_THRESH_A = 0.008f;
constexpr int CHARGE_DEBOUNCE_TICKS = 2;
inline float calc_soc(float v, float i) {
  float v_ocv = v + i * BAT_R_INT_OHM;
  const float cv[] = {2.90f,3.20f,3.40f,3.60f,3.70f,3.80f,3.85f,3.90f,3.95f,4.00f,4.05f,4.10f,4.15f,4.20f};
  const float cs[] = { 0.0f, 1.0f, 5.0f,15.0f,25.0f,40.0f,50.0f,60.0f,70.0f,80.0f,88.0f,94.0f,98.0f,100.0f};
  const int N = 14;
  if (v_ocv <= cv[0])    return 0.0f;
  if (v_ocv >= cv[N-1])  return 100.0f;
  for (int k = 1; k < N; k++) {
    if (v_ocv <= cv[k]) {
      float t = (v_ocv - cv[k-1]) / (cv[k] - cv[k-1]);
      float soc = cs[k-1] + t * (cs[k] - cs[k-1]);
      return soc < 0.0f ? 0.0f : (soc > 100.0f ? 100.0f : soc);
    }
  }
  return 100.0f;
}
inline bool update_charging_state(float i, bool was_charging) {
  return was_charging ? (i < BAT_CHARGE_EXIT_A) : (i < BAT_CHARGE_ENTER_A);
}
inline bool debounce_charging_state(bool candidate, bool confirmed, int &counter) {
  if (candidate == confirmed) { counter = 0; return confirmed; }
  counter++;
  if (counter >= CHARGE_DEBOUNCE_TICKS) { counter = 0; return candidate; }
  return confirmed;
}
static float soc_coulomb = -1, last_idle_soc = -1;
static bool is_charging_prev = false;
static int charge_debounce_cnt = 0;
static uint32_t charge_end_ms = 0;
static void reset() { soc_coulomb = -1; last_idle_soc = -1; is_charging_prev = false; charge_debounce_cnt = 0; charge_end_ms = 0; }
// the 2 s coulomb interval + the "Battery Level" template, millis() passed in
static float tick(float v, float i, float cap, uint32_t millis_now) {
  bool cap_valid = (!std::isnan(cap) && cap > 100.0f);
  bool charge_candidate = update_charging_state(i, is_charging_prev);
  bool charging = debounce_charging_state(charge_candidate, is_charging_prev, charge_debounce_cnt);
  if (charging) {
    if (!is_charging_prev) {
      if (soc_coulomb >= 0.0f) {
      } else if (last_idle_soc >= 0.0f) {
        soc_coulomb = last_idle_soc;
      } else {
        soc_coulomb = calc_soc(v, i);
      }
      is_charging_prev = true;
    } else if (cap_valid && soc_coulomb >= 0.0f) {
      if (v >= 4.15f && i > -0.05f) {
        soc_coulomb = 100.0f;
      } else {
        float dq = (-i * 1000.0f) * 2.0f / 3600.0f;
        soc_coulomb = std::min(100.0f, soc_coulomb + dq / cap * 100.0f);
      }
    }
  } else {
    if (is_charging_prev) charge_end_ms = millis_now;
    is_charging_prev = false;
    if (soc_coulomb >= 0.0f) {
      if ((uint32_t) (millis_now - charge_end_ms) < SOC_HOLD_AFTER_CHARGE_MS) {
        if (cap_valid) {
          float dq = (-i * 1000.0f) * 2.0f / 3600.0f;
          soc_coulomb = std::max(0.0f, std::min(100.0f, soc_coulomb + dq / cap * 100.0f));
        }
      } else {
        soc_coulomb = -1.0f;
      }
    } else if (!charge_candidate) {
      last_idle_soc = calc_soc(v, i);
    }
  }
  if (soc_coulomb >= 0.0f) return soc_coulomb;
  return calc_soc(v, i);
}
static const char *status(float c) {
  if (is_charging_prev) return "Charging";
  if (c > BAT_DISCHARGE_THRESH_A) return "Discharging";
  return "Idle";
}
}  // namespace legacy_hs

// ------------------------------------------------------------------ tests
static Config sensor_cfg() {
  Config c;
  c.curve = builtin_curve(Chemistry::LIPO);
  c.critical_v = 3.50f;
  c.resume_v = 3.65f;
  return c;
}
static Config scales_cfg(float cap) {
  Config c;
  c.curve = builtin_curve(Chemistry::LIION);
  c.capacity_mah = cap;
  c.internal_resistance = 0.14f;
  c.smoothing = 1.0f;
  c.only_falls = false;
  c.round_percent = false;
  return c;
}

static void test_curves() {
  const Curve lipo = builtin_curve(Chemistry::LIPO);
  for (float v = 3.0f; v < 4.4f; v += 0.0007f) CHECK_NEAR(pct_from_voltage(lipo, v), legacy_ms::lipo_pct(v), 1e-4);
  const Curve li = builtin_curve(Chemistry::LIION);
  for (float v = 2.6f; v < 4.4f; v += 0.0007f) CHECK_NEAR(pct_from_voltage(li, v), legacy_hs::calc_soc(v, 0), 1e-4);
  for (Chemistry ch : {Chemistry::LIPO, Chemistry::LIION, Chemistry::LIFEPO4}) {
    const Curve c = builtin_curve(ch);
    for (size_t k = 1; k < c.n; k++) CHECK(c.p[k].v > c.p[k - 1].v && c.p[k].pct >= c.p[k - 1].pct);
    for (float p = 0; p <= 100; p += 0.5f) CHECK_NEAR(pct_from_voltage(c, voltage_from_pct(c, p)), p, 0.02);
  }
  CHECK(std::isnan(pct_from_voltage(lipo, NAN)));
}

// Voltage only: random walks of rest voltage incl. charger jumps, many seeds -> same %, flags, voltages
static void test_sensor_equivalence() {
  std::mt19937 rng(1234);
  std::normal_distribution<float> noise(0, 0.02f);
  std::uniform_real_distribution<float> u(0, 1);
  for (int run = 0; run < 400; run++) {
    legacy_ms::reset();
    State st;
    Estimator e(sensor_cfg(), &st);
    float v = 3.3f + u(rng) * 0.95f;
    for (int k = 0; k < 600; k++) {
      v -= 0.0004f + (u(rng) < 0.01f ? 0.05f : 0);
      if (u(rng) < 0.01f) v += 0.12f + u(rng) * 0.4f;   // charger
      if (v < 3.0f) v = 3.0f;
      if (v > 4.25f) v = 4.25f;
      const float meas = v + noise(rng);
      float tx, rest;
      const int lp = legacy_ms::step(meas, &tx, &rest);
      const float np = e.update_voltage(meas);
      CHECK_NEAR(np, lp, 1e-6);
      CHECK_NEAR(st.vf, rest, 1e-6);
      CHECK_NEAR(e.report_voltage(), tx, 1e-6);
      CHECK(st.low == legacy_ms::bat_low);
      CHECK(st.critical == legacy_ms::bat_crit);
      if (g_fail > 20) return;
    }
  }
}

// Voltage + current: solar days with clouds, charge tail, sleep gaps
static void test_scales_equivalence() {
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> u(0, 1);
  for (int run = 0; run < 200; run++) {
    legacy_hs::reset();
    State st;
    const float cap = u(rng) < 0.15f ? 0 : 8700;
    Estimator e(scales_cfg(cap), &st);
    float soc = 20 + u(rng) * 70;   // "true" charge, only to drive v/i
    uint32_t t = 1000 + (uint32_t) (u(rng) * 5000);
    for (int k = 0; k < 3000; k++) {
      t += 2000;
      const int phase = (k / 300) % 4;   // idle, charge, idle, discharge
      float i = 0.02f;
      if (phase == 1) i = u(rng) < 0.1f ? -0.002f : -0.3f - u(rng) * 0.9f;   // clouds dip to ~0
      if (phase == 3) i = 0.05f + u(rng) * 0.3f;
      if (phase == 0 || phase == 2) i = (u(rng) - 0.5f) * 0.012f;
      soc = fmaxf(0, fminf(100, soc - i * 2 / 3600 * 1000 / 87));
      float v = voltage_from_pct(builtin_curve(Chemistry::LIION), soc) - i * 0.14f + (u(rng) - 0.5f) * 0.004f;
      if (phase == 1 && soc > 97) { v = 4.17f; i = -0.03f; }   // charge taper
      const float lp = legacy_hs::tick(v, i, cap, t);
      const float np = e.update(v, i, (int64_t) t);
      CHECK_NEAR(np, lp, 2e-4);
      CHECK(st.charging == legacy_hs::is_charging_prev);
      CHECK(std::string(status_str(st.status)) == legacy_hs::status(i));
      if (g_fail > 20) return;
    }
  }
}

static void test_series_parallel_power() {
  Config c = scales_cfg(2900);
  c.cells_parallel = 3;
  c.cells_series = 2;
  State st;
  Estimator e(c, &st);
  CHECK_NEAR(e.capacity_total_mah(), 8700, 1e-3);
  const float p = e.update(2 * 3.85f, 0.0f, 1000);
  CHECK_NEAR(p, 50, 1e-3);   // 3.85 V per cell = 50 % on the Li-ion curve
  CHECK_NEAR(e.remaining_mah(), 4350, 0.5);
  CHECK_NEAR(Estimator::power(7.7f, 0.5f), 3.85, 1e-5);
  CHECK_NEAR(Estimator::current_from_power(3.85f, 7.7f), 0.5, 1e-6);
  CHECK(std::isnan(Estimator::current_from_power(1, 0.1f)));
  // time left: 4350 mAh at 0.5 A = 8.7 h
  CHECK_NEAR(e.time_left_h(0.5f), 8.7, 1e-3);
  CHECK(std::isnan(e.time_left_h(-0.2f)));
}

static void test_rescale_and_nan() {
  State st;
  Estimator e(sensor_cfg(), &st);
  e.update_voltage(3.90f);
  e.rescale(1.01f);
  CHECK_NEAR(st.vf, 3.90f * 1.01f, 1e-5);
  const float p = st.percent;
  CHECK_NEAR(e.update_voltage(NAN), p, 0);
  CHECK(st.valid());
  st.magic = 0;
  CHECK(!st.valid());
}


// ---- scenarios with explicit expectations
static void test_voltage_only_rules() {
  State st;
  Estimator e(sensor_cfg(), &st);
  CHECK_NEAR(e.update_voltage(3.85f), 55, 0);           // first reading taken as is
  CHECK_NEAR(e.update_voltage(3.87f), 55, 0);           // small rise without charger: percent stays
  CHECK_NEAR(e.update_voltage(3.84f), 55, 0);           // smoothing: one low reading barely moves it
  for (int k = 0; k < 40; k++) e.update_voltage(3.80f);
  CHECK_NEAR(st.percent, 40, 1);                         // ... a steady fall gets through
  CHECK_NEAR(e.update_voltage(4.00f), 78, 1);           // jump > 0.10 V = charger: accepted at once
  // low / critical with hysteresis
  State s2;
  Estimator e2(sensor_cfg(), &s2);
  e2.update_voltage(3.69f);
  CHECK(s2.low && !s2.critical);                         // 10 %
  e2.update_voltage(3.70f);
  CHECK(s2.low);                                         // 12 %: still low (clears at 15)
  e2.update_voltage(3.49f);
  CHECK(!s2.critical);                                   // one noisy low reading is smoothed away
  for (int k = 0; k < 30; k++) e2.update_voltage(3.49f);
  CHECK(s2.critical);                                    // a steady one gets through
  for (int k = 0; k < 30; k++) e2.update_voltage(3.60f);
  CHECK(s2.critical);                                    // below resume 3.65
  e2.update_voltage(3.80f);                              // recharged (jump)
  CHECK(!s2.critical && !s2.low);
}

static void test_report_voltage() {
  State st;
  Estimator e(sensor_cfg(), &st);
  e.update_voltage(4.00f);
  CHECK_NEAR(e.report_voltage(), 4.00, 1e-6);
  e.update_voltage(4.02f);                               // avg 4.01: change < 0.03 -> keeps 4.00
  CHECK_NEAR(e.report_voltage(), 4.00, 1e-6);
  for (int k = 0; k < 10; k++) e.update_voltage(4.05f);
  CHECK_NEAR(e.report_voltage(), 4.03, 1e-6);            // moved once by >= 0.03, then 4.05 is < 0.03 away
  for (int k = 0; k < 10; k++) e.update_voltage(4.07f);
  CHECK_NEAR(e.report_voltage(), 4.07, 0.0301);          // within one step of the true value
  e.update_voltage(3.80f);                               // jump > 0.15: average restarts
  CHECK_NEAR(e.report_voltage(), 3.80, 1e-6);
}

static void test_charge_cycle() {
  Config c = scales_cfg(2000);
  State st;
  Estimator e(c, &st);
  int64_t t = 1000;
  for (int k = 0; k < 5; k++) e.update(3.80f, 0.0f, t += 2000);   // rest at 40 %
  CHECK_NEAR(st.percent, 40, 0.01);
  CHECK(st.status == Status::IDLE);
  e.update(3.95f, -1.0f, t += 2000);                     // 1 A charge, first update: debounce
  CHECK(!st.charging);
  e.update(3.95f, -1.0f, t += 2000);
  CHECK(st.charging && st.status == Status::CHARGING);
  CHECK_NEAR(st.percent, 40, 0.01);                       // starts from the last rest value, no jump
  for (int k = 0; k < 1800; k++) e.update(3.95f, -1.0f, t += 2000);   // 1 h at 1 A = 1000 mAh
  CHECK_NEAR(st.percent, 90, 0.2);                        // 40 % + 1000/2000 -> 90 %
  e.update(4.16f, -0.03f, t += 2000);                     // tapered off at full voltage
  CHECK_NEAR(st.percent, 100, 0);
  e.update(4.10f, 0.0f, t += 2000);
  e.update(4.10f, 0.0f, t += 2000);
  CHECK(!st.charging);
  CHECK_NEAR(st.percent, 100, 0.01);                      // hold: still counting
  e.update(4.10f, 0.0f, t += 21 * 60 * 1000);             // gap > max_dt: not integrated, hold expired
  CHECK_NEAR(st.percent, 94, 0.01);                       // back to the curve: 4.10 V -> 94 %
  CHECK(st.soc_cc < 0);
}

static void test_sign_trigger_and_no_capacity() {
  Config c = scales_cfg(0);                               // capacity unknown
  c.charge_enter_a = 0;
  c.charge_exit_a = 0;
  c.debounce = 1;
  State st;
  Estimator e(c, &st);
  e.update(3.80f, -0.001f, 1000);
  CHECK(st.charging);                                     // strictly by the sign, at once
  const float p = st.percent;
  e.update(3.85f, -0.5f, 3000);
  CHECK_NEAR(st.percent, p, 0);                           // no capacity: count holds, no integration
  e.update(3.80f, 0.001f, 5000);
  CHECK(!st.charging);
  CHECK(std::isnan(e.remaining_mah()));
}

static void test_ir_compensation_and_lifepo4() {
  Config c = scales_cfg(2900);
  State st;
  Estimator e(c, &st);
  e.update(3.80f - 0.5f * 0.14f, 0.5f, 1000);             // 0.5 A load drops V by I*R
  CHECK_NEAR(st.ocv, 3.80, 1e-4);
  CHECK_NEAR(st.percent, 40, 0.01);
  Config lf;
  lf.curve = builtin_curve(Chemistry::LIFEPO4);
  lf.cells_series = 4;
  lf.critical_v = 2.90f;
  lf.resume_v = 3.10f;
  State s2;
  Estimator e2(lf, &s2);
  CHECK_NEAR(e2.update_voltage(4 * 3.26f), 50, 0);
  CHECK_NEAR(s2.ocv, 13.04, 1e-4);
}

static void test_custom_curve() {
  static const CurvePoint pts[] = {{3.0f, 0}, {3.7f, 50}, {4.2f, 100}};
  Config c;
  c.curve = {pts, 3};
  c.smoothing = 1;
  c.only_falls = false;
  State st;
  Estimator e(c, &st);
  CHECK_NEAR(e.update_voltage(3.35f), 25, 0);
  CHECK_NEAR(e.update_voltage(3.95f), 75, 0);
  CHECK_NEAR(voltage_from_pct(c.curve, 75), 3.95, 1e-5);
}

int main() {
  test_curves();
  test_sensor_equivalence();
  test_scales_equivalence();
  test_series_parallel_power();
  test_rescale_and_nan();
  test_voltage_only_rules();
  test_report_voltage();
  test_charge_cycle();
  test_sign_trigger_and_no_capacity();
  test_ir_compensation_and_lifepo4();
  test_custom_curve();
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
