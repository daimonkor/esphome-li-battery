#include "li_battery.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include <sys/time.h>

#ifdef USE_ESP32
#include <esp_attr.h>
#endif

namespace esphome {
namespace li_battery {

static const char *const TAG = "li_battery";

#ifdef USE_ESP32
// State kept through deep sleep (restore: rtc). One slot per li_battery instance.
static constexpr uint8_t RTC_SLOTS = 2;
static RTC_DATA_ATTR State rtc_states[RTC_SLOTS];
static uint8_t rtc_used = 0;
#endif

// Time that keeps running through deep sleep on ESP32 (RTC timer); elsewhere millis().
static int64_t now_ms() {
#ifdef USE_ESP32
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
#else
  return (int64_t) millis();
#endif
}

static const char *chem_str(Chemistry c) {
  switch (c) {
    case Chemistry::LIION: return "liion";
    case Chemistry::LIFEPO4: return "lifepo4";
    case Chemistry::CUSTOM: return "custom";
    default: return "lipo";
  }
}

void LiBattery::setup() {
  this->state_ = &this->ram_state_;
#ifdef USE_ESP32
  if (this->restore_ == Restore::RTC) {
    if (rtc_used < RTC_SLOTS) {
      this->state_ = &rtc_states[rtc_used++];
      if (!this->state_->valid()) this->state_->reset();
    } else {
      ESP_LOGW(TAG, "restore: rtc — only %u instances can keep RTC state, this one uses RAM", RTC_SLOTS);
    }
  }
#endif
  if (this->chemistry_ == Chemistry::CUSTOM && this->custom_curve_.size() >= 2) {
    this->est_.cfg.curve = {this->custom_curve_.data(), this->custom_curve_.size()};
  } else {
    this->est_.cfg.curve = ::li_battery_core::builtin_curve(this->chemistry_);
  }
  this->est_.st = this->state_;
  this->apply_params_();

  if (this->voltage_ != nullptr && !this->has_current_input_()) {
    // voltage only: every new reading is processed at once
    this->voltage_->add_on_state_callback([this](float v) {
      this->process_(v, NAN);
      this->publish_();
    });
  }
  // restored values from before the sleep
  if (!std::isnan(this->state_->percent)) this->publish_();
}

void LiBattery::update() {
  if (this->voltage_ == nullptr || !this->has_current_input_()) return;
  if (!this->voltage_->has_state()) return;
  const float v = this->voltage_->state;
  float i = NAN;
  if (this->current_ != nullptr && this->current_->has_state()) {
    i = this->current_->state;
  } else if (this->power_in_ != nullptr && this->power_in_->has_state()) {
    i = Estimator::current_from_power(this->power_in_->state, v);
  }
  if (std::isnan(i)) i = 0;   // no current reading yet: treat as rest
  this->process_(v, i);
  this->publish_();
}

float LiBattery::process_voltage(float v) {
  if (this->state_ == nullptr) return NAN;
  this->process_(v, NAN);
  this->publish_();
  return this->state_->percent;
}

void LiBattery::reset() {
  if (this->state_ == nullptr) return;
  this->state_->reset();
  ESP_LOGI(TAG, "state reset");
}

void LiBattery::apply_params_() {
  Config &c = this->est_.cfg;
  const float cap = this->capacity_.get();
  c.capacity_mah = std::isnan(cap) ? 0 : cap;
  const float r = this->r_int_.get();
  c.internal_resistance = std::isnan(r) ? 0 : r;
  const float s = this->cells_series_.get();
  c.cells_series = (std::isnan(s) || s < 1) ? 1 : (uint8_t) lroundf(s);
  const float p = this->cells_parallel_.get();
  c.cells_parallel = (std::isnan(p) || p < 1) ? 1 : (uint8_t) lroundf(p);
  const float lo = this->low_.get();
  if (!std::isnan(lo)) {
    c.low_pct = lo;
    c.low_clear_pct = lo + 5;
  }
  c.critical_v = this->critical_.get();
  c.resume_v = this->resume_.get();
}

void LiBattery::process_(float v, float i) {
  this->apply_params_();
  const bool was_low = this->state_->low, was_crit = this->state_->critical, was_chg = this->state_->charging;
  if (std::isnan(i)) {
    this->est_.update_voltage(v, now_ms());
  } else {
    this->est_.update(v, i, now_ms());
  }
  this->last_i_ = i;
  const State &st = *this->state_;
  if (st.low != was_low) ESP_LOGW(TAG, "battery low %s (%.0f %%)", st.low ? "ON" : "OFF", st.percent);
  if (st.critical != was_crit)
    ESP_LOGW(TAG, "battery critical %s (%.3f V)", st.critical ? "ON" : "OFF", st.ocv);
  if (st.charging != was_chg) ESP_LOGI(TAG, "charging %s", st.charging ? "started" : "ended");
  ESP_LOGD(TAG, "V %.3f I %.3f -> rest %.3f V, %.1f %% (curve %.1f %%)%s%s%s%s", v, i, st.ocv, st.percent, st.pct_true,
           st.low ? " LOW" : "", st.critical ? " CRITICAL" : "", st.charging ? " CHARGING" : "",
           st.full_relax ? " FULL-HOLD" : "");
}

void LiBattery::publish_() {
  const State &st = *this->state_;
  if (this->percent_s_ != nullptr && !std::isnan(st.percent)) this->percent_s_->publish_state(st.percent);
  if (this->voltage_s_ != nullptr && !std::isnan(st.ocv)) this->voltage_s_->publish_state(st.ocv);
  if (this->report_s_ != nullptr) {
    const float rv = this->est_.report_voltage();
    if (!std::isnan(rv) && (!this->report_s_->has_state() || this->report_s_->state != rv))
      this->report_s_->publish_state(rv);
  }
  if (this->remaining_s_ != nullptr) {
    const float r = this->est_.remaining_mah();
    if (!std::isnan(r)) this->remaining_s_->publish_state(r);
  }
  // with a current: at that current; voltage only with charge_detect: at the discharge rate
  if (this->time_left_s_ != nullptr && (!std::isnan(this->last_i_) || this->est_.cfg.charge_detect))
    this->time_left_s_->publish_state(this->est_.time_left_h(this->last_i_));
  if (this->power_s_ != nullptr) {
    // a power sensor given as input is passed through as is; otherwise V x I
    if (this->power_in_ != nullptr && this->power_in_->has_state()) {
      this->power_s_->publish_state(this->power_in_->state);
    } else if (!std::isnan(this->last_i_) && this->voltage_ != nullptr && this->voltage_->has_state()) {
      this->power_s_->publish_state(Estimator::power(this->voltage_->state, this->last_i_));
    }
  }
#ifdef USE_BINARY_SENSOR
  if (this->charging_s_ != nullptr && (!std::isnan(this->last_i_) || this->est_.cfg.charge_detect))
    this->charging_s_->publish_state(st.charging);
  if (this->low_s_ != nullptr && !std::isnan(st.percent)) this->low_s_->publish_state(st.low);
  if (this->critical_s_ != nullptr && !std::isnan(st.ocv)) this->critical_s_->publish_state(st.critical);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->status_s_ != nullptr && (!std::isnan(this->last_i_) || this->est_.cfg.charge_detect)) {
    const std::string s = ::li_battery_core::status_str(st.status);
    if (!this->status_s_->has_state() || this->status_s_->state != s) this->status_s_->publish_state(s);
  }
#endif
}

void LiBattery::dump_config() {
  const Config &c = this->est_.cfg;
  ESP_LOGCONFIG(TAG, "Li battery:");
  ESP_LOGCONFIG(TAG, "  Chemistry: %s (%u curve points), %uS%uP", chem_str(this->chemistry_), (unsigned) c.curve.n,
                c.cells_series, c.cells_parallel);
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->has_current_input_() ? "voltage + current" : "voltage only");
  ESP_LOGCONFIG(TAG, "  Capacity: %.0f mAh per cell (%.0f mAh total)", c.capacity_mah, this->est_.capacity_total_mah());
  ESP_LOGCONFIG(TAG, "  Internal resistance: %.3f ohm", c.internal_resistance);
  ESP_LOGCONFIG(TAG, "  Low: %.0f %% (clears at %.0f %%), critical: %.2f V, resume: %.2f V", c.low_pct, c.low_clear_pct,
                c.critical_v, c.resume_v);
  if (!this->has_current_input_())
    ESP_LOGCONFIG(TAG, "  Charge detect: %s (charging at %.3f V per cell, jump %.3f V)", YESNO(c.charge_detect), c.charge_v,
                  c.charge_jump_v);
  if (!this->has_current_input_() && c.charge_detect)
    ESP_LOGCONFIG(TAG, "  Full hold: %s (%.1f h, max gap %.0f %%)", YESNO(c.full_anchor), c.full_relax_ms / 3600000.0f,
                  c.full_max_gap_pct);
  ESP_LOGCONFIG(TAG, "  Restore: %s", this->restore_ == Restore::RTC ? "rtc" : "none");
  if (this->has_current_input_()) LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Percent", this->percent_s_);
  LOG_SENSOR("  ", "Rest voltage", this->voltage_s_);
  LOG_SENSOR("  ", "Report voltage", this->report_s_);
  LOG_SENSOR("  ", "Remaining", this->remaining_s_);
  LOG_SENSOR("  ", "Time left", this->time_left_s_);
  LOG_SENSOR("  ", "Power", this->power_s_);
}

#ifdef USE_NUMBER
void LiBatteryNumber::setup() {
  float value = this->initial_value_;
  if (std::isnan(value)) value = this->traits.get_min_value();
  if (this->restore_value_) {
    this->pref_ = this->make_entity_preference<float>();
    float stored;
    if (this->pref_.load(&stored)) value = stored;
  }
  this->publish_state(value);
}

void LiBatteryNumber::control(float value) {
  if (this->restore_value_) this->pref_.save(&value);
  this->publish_state(value);
}
#endif

}  // namespace li_battery
}  // namespace esphome
