#pragma once
// Copies of the code li_battery replaces (Move Sensor BLE firmware, HiveScale scales), for tests.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

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

