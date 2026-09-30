#pragma once

// Detector model. DetectorConfig is the immutable [[detectors]] entry;
// DetectorSet owns the runtime state (active, isotope, control readbacks) and
// reports every change as a DetectorState event. Config is never mutated at
// runtime.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/spectrometer/types.hpp"

namespace pychron::spectrometer {

// Selects host-integration math and the applicable IDetectorControl caps.
enum class DetectorKind { Faraday, Counter, Cdd, Atona };

// "faraday", "counter", "cdd", "atona".
std::string_view to_string(DetectorKind kind) noexcept;
Result<DetectorKind> parse_detector_kind(std::string_view text);

// IDetectorControl caps that make sense for `kind`.
Caps applicable_caps(DetectorKind kind) noexcept;

struct DeflectionConfig {
  bool control = false;
  std::vector<double> correction;  // polynomial coefficients, lowest order first
  int sign = 1;
  std::optional<double> max;       // |deflection| clamp
  double per_volt = 0.0;

  // `value` clamped to [-max, max] when max is set.
  double clamp(double value) const noexcept;
};

struct ProtectionConfig {
  double threshold = 0.0;
  bool on_move = true;
};

struct DetectorConfig {
  DetectorId name;
  DetectorKind kind = DetectorKind::Faraday;
  ChannelId channel;
  std::string units;
  double software_gain = 1.0;
  std::string isotope;
  bool active = true;
  std::optional<DeflectionConfig> deflection;
  std::optional<ProtectionConfig> protection;
  std::optional<double> saturation;
  std::optional<double> dead_time_ns;
  std::optional<double> cdd_voltage;
};

// Runtime state of one detector; published on change.
struct DetectorState {
  DetectorId detector;
  bool active = true;
  std::string isotope;
  bool protected_ = false;
  std::optional<double> deflection;
  std::optional<double> gain;
  std::optional<double> cdd_voltage;
  TimePoint ts{};

  friend bool operator==(const DetectorState&, const DetectorState&) = default;
};

// Not thread-safe: owned and serialized by the systems-layer Spectrometer.
class DetectorSet {
 public:
  using Listener = std::function<void(const DetectorState&)>;

  // Config error (all problems joined) for empty/duplicate names or channels.
  // States start from config (`active`, `isotope`, `cdd_voltage`). `clock`
  // stamps DetectorState::ts; without it ts stays default.
  static Result<DetectorSet> create(std::vector<DetectorConfig> configs,
                                    const Clock* clock = nullptr);

  std::size_t size() const noexcept { return configs_.size(); }
  const std::vector<DetectorConfig>& configs() const noexcept { return configs_; }

  const DetectorConfig* find(std::string_view name) const noexcept;
  const DetectorConfig* by_channel(std::string_view channel) const noexcept;

  Result<DetectorState> state(std::string_view name) const;
  std::vector<DetectorState> states() const;
  // Names of active detectors, config order.
  std::vector<DetectorId> active() const;

  // Each mutator publishes the new state to the listener if anything changed;
  // unknown detectors fail with Config.
  Result<void> set_active(std::string_view name, bool active);
  Result<void> set_isotope(std::string_view name, std::string isotope);
  Result<void> record_protected(std::string_view name, bool on);
  Result<void> record_deflection(std::string_view name, double value);
  Result<void> record_gain(std::string_view name, double value);
  Result<void> record_cdd_voltage(std::string_view name, double volts);

  void on_change(Listener listener) { listener_ = std::move(listener); }

 private:
  DetectorSet(std::vector<DetectorConfig> configs, const Clock* clock);

  Result<std::size_t> index_of(std::string_view name) const;
  template <class Mutate>
  Result<void> update(std::string_view name, Mutate mutate);

  std::vector<DetectorConfig> configs_;
  std::vector<DetectorState> states_;
  const Clock* clock_ = nullptr;
  Listener listener_;
};

}  // namespace pychron::spectrometer
