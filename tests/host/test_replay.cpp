// Replay of real INA226 data from the HiveScale scales (Home Assistant history, 7-13 Nov 2026):
// 26 080 ticks of 2 s = ~14.5 h awake in 121 wake segments; charging (up to 4.3 A, ~24 min of it awake),
// discharge down to 2.98 V.
// Each row: t_ms, voltage (V), current (A, + discharge), segment (a new one after sleep / offline),
// ha_level = the percent the scales themselves reported to HA at that moment.
//
//   make -C tests/host replay
//
// Checks: (1) li_battery == the scales' own code on the same inputs, every tick;
//         (2) li_battery vs what the scales actually reported (timing differs by up to one tick).
#include "../../components/li_battery/li_battery_core.h"
#include "legacy.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace li_battery_core;

struct Row { long long t; float v, i; int seg; float ha; };

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "ina226.csv";
  FILE *f = fopen(path, "r");
  if (!f) { std::printf("no data file %s\n", path); return 1; }
  std::vector<Row> rows;
  char line[256];
  if (!fgets(line, sizeof line, f)) { fclose(f); return 1; }   // header
  while (fgets(line, sizeof line, f)) {
    Row r{};
    char ha[32] = {0};
    if (std::sscanf(line, "%lld,%f,%f,%d,%31s", &r.t, &r.v, &r.i, &r.seg, ha) < 4) continue;
    r.ha = ha[0] ? std::strtof(ha, nullptr) : NAN;
    rows.push_back(r);
  }
  fclose(f);

  Config c;
  c.curve = builtin_curve(Chemistry::LIION);
  c.capacity_mah = 8700;          // the scales' "Battery Capacity" number holds the whole pack
  c.internal_resistance = 0.14f;
  c.smoothing = 1.0f;
  c.only_falls = false;
  c.round_percent = false;

  int fails = 0, seg = -1, charge_ticks = 0, cc_ticks = 0, switches = 0;
  double max_legacy = 0, sum_ha = 0, max_ha = 0;
  long n_ha = 0, n_ha_far = 0;
  State st;
  Estimator e(c, &st);
  bool prev_chg = false;
  for (const Row &r : rows) {
    if (r.seg != seg) {           // woke up: RAM state starts fresh, as on the scales
      seg = r.seg;
      st.reset();
      legacy_hs::reset();
    }
    const uint32_t ms = (uint32_t) (r.t + 100000);   // millis() never 0 in the legacy code
    const float lp = legacy_hs::tick(r.v, r.i, 8700, ms);
    const float np = e.update(r.v, r.i, (int64_t) ms);
    const double d = std::fabs(lp - np);
    if (d > max_legacy) max_legacy = d;
    if (d > 2e-4 || st.charging != legacy_hs::is_charging_prev) {
      if (fails++ < 10) std::printf("MISMATCH t=%lld v=%.3f i=%.3f legacy=%.3f lib=%.3f\n", r.t, r.v, r.i, lp, np);
    }
    if (st.charging) charge_ticks++;
    if (st.soc_cc >= 0) cc_ticks++;
    if (st.charging != prev_chg) switches++;
    prev_chg = st.charging;
    if (!std::isnan(r.ha)) {
      const double h = std::fabs(np - r.ha);
      sum_ha += h; n_ha++;
      if (h > max_ha) max_ha = h;
      if (h > 1.0) n_ha_far++;
    }
  }
  std::printf("replay: %zu ticks, %d segments, charging %d ticks (%.1f h), coulomb counting %d ticks, %d switches\n",
              rows.size(), seg + 1, charge_ticks, charge_ticks * 2 / 3600.0, cc_ticks, switches);
  std::printf("li_battery vs scales code:      max diff %.6f %%\n", max_legacy);
  std::printf("li_battery vs reported to HA:   mean %.3f %%, max %.3f %%, >1 %% in %ld of %ld ticks\n",
              n_ha ? sum_ha / n_ha : 0, max_ha, n_ha_far, n_ha);
  const bool ok = fails == 0 && rows.size() > 20000 && charge_ticks > 500 && max_ha < 1.0;
  std::printf("%s\n", ok ? "OK" : "FAILED");
  return ok ? 0 : 1;
}
