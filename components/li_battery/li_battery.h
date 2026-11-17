#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/sensor/sensor.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif

#include "li_battery_core.h"

#include <vector>

namespace esphome {
namespace li_battery {

using ::li_battery_core::Chemistry;
using ::li_battery_core::Config;
using ::li_battery_core::CurvePoint;
using ::li_battery_core::Estimator;
using ::li_battery_core::State;

enum class Restore : uint8_t { NONE = 0, RTC = 1 };

// A setting that is either a fixed number or follows a number entity (changeable from HA).
struct Param {
  float value = NAN;
#ifdef USE_NUMBER
  number::Number *num = nullptr;
#endif
  float get() const {
#ifdef USE_NUMBER
    if (num != nullptr && num->has_state()) return num->state;
#endif
    return value;
  }
};

class LiBattery : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // inputs
  void set_voltage_sensor(sensor::Sensor *s) { this->voltage_ = s; }
  void set_current_sensor(sensor::Sensor *s) { this->current_ = s; }
  void set_power_sensor(sensor::Sensor *s) { this->power_in_ = s; }

  // configuration
  void set_chemistry(Chemistry c) { this->chemistry_ = c; }
  void add_curve_point(float v, float pct) { this->custom_curve_.push_back({v, pct}); }
  void set_restore(Restore r) { this->restore_ = r; }
  Param &capacity() { return this->capacity_; }
  Param &internal_resistance() { return this->r_int_; }
  Param &cells_series() { return this->cells_series_; }
  Param &cells_parallel() { return this->cells_parallel_; }
  Param &low() { return this->low_; }
  Param &critical() { return this->critical_; }
  Param &resume() { return this->resume_; }
  Config &core_config() { return this->est_.cfg; }

  // outputs
  void set_percent_sensor(sensor::Sensor *s) { this->percent_s_ = s; }
  void set_voltage_out_sensor(sensor::Sensor *s) { this->voltage_s_ = s; }
  void set_report_voltage_sensor(sensor::Sensor *s) { this->report_s_ = s; }
  void set_remaining_sensor(sensor::Sensor *s) { this->remaining_s_ = s; }
  void set_time_left_sensor(sensor::Sensor *s) { this->time_left_s_ = s; }
  void set_power_out_sensor(sensor::Sensor *s) { this->power_s_ = s; }
#ifdef USE_BINARY_SENSOR
  void set_charging_binary_sensor(binary_sensor::BinarySensor *s) { this->charging_s_ = s; }
  void set_low_binary_sensor(binary_sensor::BinarySensor *s) { this->low_s_ = s; }
  void set_critical_binary_sensor(binary_sensor::BinarySensor *s) { this->critical_s_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_s_ = s; }
#endif

  // ---- for lambdas
  float percent() const { return this->state_->percent; }
  bool is_low() const { return this->state_->low; }
  bool is_critical() const { return this->state_->critical; }
  bool is_charging() const { return this->state_->charging; }
  float rest_voltage() const { return this->state_->ocv; }
  float report_voltage() const { return this->est_.report_voltage(); }
  float remaining_mah() const { return this->est_.remaining_mah(); }
  // pack voltage at which the curve gives `pct` (e.g. a cut-off for a modem). With `current_a`
  // (A, + discharge): the voltage a meter shows at that current, OCV − I·R.
  float voltage_for_percent(float pct, float current_a = 0) const {
    const float ocv = ::li_battery_core::voltage_from_pct(this->est_.cfg.curve, pct) * this->est_.series();
    if (std::isnan(current_a)) return ocv;
    return ocv - current_a * this->est_.cfg.internal_resistance;
  }
  // feed a pack voltage directly (voltage-only mode, no input sensor), returns %
  float process_voltage(float v);
  // the voltage scale changed (divider factor edited): rescale the history
  void rescale(float ratio) { this->est_.rescale(ratio); }
  // forget everything (new battery)
  void reset();

 protected:
  void apply_params_();
  void process_(float v, float i);
  void publish_();
  bool has_current_input_() const { return this->current_ != nullptr || this->power_in_ != nullptr; }

  sensor::Sensor *voltage_{nullptr}, *current_{nullptr}, *power_in_{nullptr};
  Chemistry chemistry_{Chemistry::LIPO};
  std::vector<CurvePoint> custom_curve_;
  Restore restore_{Restore::NONE};
  Param capacity_, r_int_, cells_series_, cells_parallel_, low_, critical_, resume_;

  Estimator est_;
  State *state_{nullptr};
  State ram_state_;
  float last_i_{NAN};

  sensor::Sensor *percent_s_{nullptr}, *voltage_s_{nullptr}, *report_s_{nullptr}, *remaining_s_{nullptr},
      *time_left_s_{nullptr}, *power_s_{nullptr};
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *charging_s_{nullptr}, *low_s_{nullptr}, *critical_s_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *status_s_{nullptr};
#endif
};

#ifdef USE_NUMBER
// number created by the component itself (restored from flash), e.g. "Battery Capacity"
class LiBatteryNumber : public number::Number, public Component {
 public:
  void setup() override;
  void set_restore_value(bool r) { this->restore_value_ = r; }
  void set_initial_value(float v) { this->initial_value_ = v; }

 protected:
  void control(float value) override;
  bool restore_value_{true};
  float initial_value_{NAN};
  ESPPreferenceObject pref_;
};
#endif

}  // namespace li_battery
}  // namespace esphome
