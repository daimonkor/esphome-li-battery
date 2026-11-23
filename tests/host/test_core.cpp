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

#include "legacy.h"

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

// the scales' GSM Battery Limit: % -> OCV by the same table, then terminal = OCV - I*R
static void test_inverse_like_scales() {
  const float cs[] = { 0.0f, 1.0f, 5.0f,15.0f,25.0f,40.0f,50.0f,60.0f,70.0f,80.0f,88.0f,94.0f,98.0f,100.0f};
  const float cv[] = {2.90f,3.20f,3.40f,3.60f,3.70f,3.80f,3.85f,3.90f,3.95f,4.00f,4.05f,4.10f,4.15f,4.20f};
  const int N = 14;
  const Curve li = builtin_curve(Chemistry::LIION);
  for (float x = 0.5f; x < 100; x += 0.5f) {
    float lim_ocv = cv[N - 1];
    for (int k = 1; k < N; k++)
      if (x <= cs[k]) { lim_ocv = cv[k - 1] + (x - cs[k - 1]) / (cs[k] - cs[k - 1]) * (cv[k] - cv[k - 1]); break; }
    CHECK_NEAR(voltage_from_pct(li, x), lim_ocv, 1e-5);
  }
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

// ---- voltage-only charge detection (1.2.0), numbers from the Move Sensor BLE history (HA, 14-24.11.2026)
static Config charge_cfg() {
  Config c = sensor_cfg();
  c.charge_detect = true;
  return c;
}
static const int64_t MIN_MS = 60LL * 1000;

static void test_charge_detect_full_cycle() {
  State st;
  Estimator e(charge_cfg(), &st);
  int64_t t = 1000;
  for (int k = 0; k < 30; k++) e.update_voltage(4.07f, t += 5 * MIN_MS);   // resting, 86-88 %
  CHECK(!st.charging);
  CHECK(st.status == Status::DISCHARGING);
  const float before = st.percent;
  e.update_voltage(4.15f, t += 2 * MIN_MS);              // plugged in: +0.08 V, first reading (debounce)
  CHECK(!st.charging);
  e.update_voltage(4.15f, t += 2 * MIN_MS);
  CHECK(st.charging && st.status == Status::CHARGING);
  CHECK(st.percent > before);                             // on the charger the percent may rise
  const float on_charger[] = {4.16f, 4.17f, 4.21f, 4.17f, 4.206f, 4.167f, 4.173f, 4.159f, 4.18f, 4.17f};
  for (float v : on_charger) e.update_voltage(v, t += 2 * MIN_MS);
  CHECK(st.charging);                                     // taper noise (one 4.159) does not end it
  CHECK(!std::isnan(e.time_left_h(NAN)) == false);        // no time left while charging
  e.update_voltage(4.15f, t += 2 * MIN_MS);               // unplugged: rests at 4.15 V for hours
  CHECK(st.charging);
  e.update_voltage(4.15f, t += 2 * MIN_MS);
  CHECK(!st.charging);
  CHECK_NEAR(st.percent, 100, 0);                         // 4.15 V rest = full
  for (int k = 0; k < 100; k++) e.update_voltage(4.15f, t += 5 * MIN_MS);
  CHECK(!st.charging);                                    // 4.15 V rest is NOT charging (old 4.15 threshold was)
}

static void test_charge_detect_rebound_and_partial() {
  // rebound after Wi-Fi load (23.11: 3.98 -> 4.07): looks like a jump, but does not keep rising
  State st;
  Estimator e(charge_cfg(), &st);
  int64_t t = 1000;
  for (int k = 0; k < 40; k++) e.update_voltage(3.98f, t += MIN_MS);
  for (int k = 0; k < 3; k++) e.update_voltage(4.07f, t += MIN_MS);
  CHECK(st.charging);                                     // taken as plugged in at first ...
  for (int k = 0; k < 35; k++) e.update_voltage(4.07f, t += MIN_MS);
  CHECK(!st.charging);                                    // ... dropped after 30 min without a climb
  for (int k = 0; k < 60; k++) e.update_voltage(4.07f, t += MIN_MS);
  CHECK(!st.charging);                                    // and not started again (vf caught up)

  // part charge, unplugged before reaching charge_v
  State s2;
  Estimator e2(charge_cfg(), &s2);
  t = 1000;
  for (int k = 0; k < 20; k++) e2.update_voltage(3.80f, t += 5 * MIN_MS);
  float v = 3.86f;
  for (int k = 0; k < 20; k++, v += 0.01f) e2.update_voltage(v, t += 2 * MIN_MS);   // up to 4.05 V
  CHECK(s2.charging);
  e2.update_voltage(3.99f, t += 2 * MIN_MS);
  e2.update_voltage(3.98f, t += 2 * MIN_MS);
  CHECK(!s2.charging);
  CHECK_NEAR(s2.vf, 3.98f, 1e-5);                         // history restarts from the rest reading
}

static void test_charge_detect_off_and_stale() {
  State st;
  Estimator e(sensor_cfg(), &st);                         // charge_detect off: old behaviour
  for (int k = 0; k < 5; k++) e.update_voltage(4.20f, 1000 + k * MIN_MS);
  CHECK(!st.charging && st.status == Status::UNKNOWN);
  CHECK(std::isnan(e.time_left_h(NAN)));

  State s2;
  Estimator e2(charge_cfg(), &s2);
  e2.update_voltage(4.20f, 1000);
  e2.update_voltage(4.20f, 1000 + MIN_MS);
  CHECK(s2.charging);
  e2.update_voltage(4.10f, 1000 + 4 * 60 * MIN_MS);       // nothing for 4 h: old state dropped, plain reading
  CHECK(!s2.charging);
}

static void test_discharge_rate() {
  State st;
  Estimator e(charge_cfg(), &st);
  int64_t t = 1000;
  e.update_voltage(4.20f, t += MIN_MS);
  e.update_voltage(4.20f, t += MIN_MS);
  CHECK(st.charging);
  e.update_voltage(4.15f, t += MIN_MS);
  e.update_voltage(4.15f, t += MIN_MS);
  CHECK(!st.charging);
  CHECK(std::isnan(e.discharge_rate_pct_h()));
  // 4.15 -> 4.10 V over 24 h (100 -> 94 %)
  for (int k = 1; k <= 288; k++) e.update_voltage(4.15f - 0.05f * k / 288.0f, t += 5 * MIN_MS);
  CHECK(!std::isnan(e.discharge_rate_pct_h()));
  CHECK_NEAR(e.discharge_rate_pct_h(), 6.0f / 24.0f, 0.06);
  CHECK_NEAR(e.time_left_h(NAN), st.percent / e.discharge_rate_pct_h(), 1e-3);
  CHECK(e.time_left_h(NAN) > 300 && e.time_left_h(NAN) < 500);
  // no clock: rate stays unknown, charge detection still works by readings
  State s2;
  Estimator e2(charge_cfg(), &s2);
  e2.update_voltage(4.20f);
  e2.update_voltage(4.20f);
  CHECK(s2.charging);
  CHECK(std::isnan(e2.discharge_rate_pct_h()));
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
  test_inverse_like_scales();
  test_charge_detect_full_cycle();
  test_charge_detect_rebound_and_partial();
  test_charge_detect_off_and_stale();
  test_discharge_rate();
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
