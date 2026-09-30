#include "pychron/devices/spectrometer/detectors.hpp"

#include <set>

namespace pychron::spectrometer {

std::string_view to_string(DetectorKind kind) noexcept {
  switch (kind) {
    case DetectorKind::Faraday: return "faraday";
    case DetectorKind::Counter: return "counter";
    case DetectorKind::Cdd: return "cdd";
    case DetectorKind::Atona: return "atona";
  }
  return "faraday";
}

Result<DetectorKind> parse_detector_kind(std::string_view text) {
  for (auto kind : {DetectorKind::Faraday, DetectorKind::Counter, DetectorKind::Cdd,
                    DetectorKind::Atona}) {
    if (to_string(kind) == text) return kind;
  }
  return fail(ErrorKind::Config, "unknown detector kind '" + std::string(text) + "'");
}

Caps applicable_caps(DetectorKind kind) noexcept {
  switch (kind) {
    case DetectorKind::Faraday:
      return DetectorCap::Gain | DetectorCap::Deflection | DetectorCap::Protect;
    case DetectorKind::Counter: return DetectorCap::Deflection | DetectorCap::Protect;
    case DetectorKind::Cdd:
      return Caps(DetectorCap::Deflection) | DetectorCap::Protect | DetectorCap::CddVoltage;
    case DetectorKind::Atona: return DetectorCap::Deflection | DetectorCap::Protect;
  }
  return {};
}

double DeflectionConfig::clamp(double value) const noexcept {
  if (!max) return value;
  return Range{-*max, *max}.clamp(value);
}

Result<DetectorSet> DetectorSet::create(std::vector<DetectorConfig> configs, const Clock* clock) {
  std::string problems;
  auto problem = [&](const std::string& p) {
    if (!problems.empty()) problems += "; ";
    problems += p;
  };
  std::set<std::string, std::less<>> names;
  std::set<std::string, std::less<>> channels;
  for (const auto& c : configs) {
    if (c.name.empty()) problem("detector with empty name");
    else if (!names.insert(c.name).second) problem("duplicate detector '" + c.name + "'");
    if (c.channel.empty()) problem("detector '" + c.name + "' has no channel");
    else if (!channels.insert(c.channel).second)
      problem("channel '" + c.channel + "' bound to more than one detector");
  }
  if (!problems.empty()) return fail(ErrorKind::Config, problems);
  return DetectorSet(std::move(configs), clock);
}

DetectorSet::DetectorSet(std::vector<DetectorConfig> configs, const Clock* clock)
    : configs_(std::move(configs)), clock_(clock) {
  const TimePoint ts = clock_ ? clock_->now() : TimePoint{};
  states_.reserve(configs_.size());
  for (const auto& c : configs_) {
    DetectorState s;
    s.detector = c.name;
    s.active = c.active;
    s.isotope = c.isotope;
    s.cdd_voltage = c.cdd_voltage;
    s.ts = ts;
    states_.push_back(std::move(s));
  }
}

const DetectorConfig* DetectorSet::find(std::string_view name) const noexcept {
  for (const auto& c : configs_) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

const DetectorConfig* DetectorSet::by_channel(std::string_view channel) const noexcept {
  for (const auto& c : configs_) {
    if (c.channel == channel) return &c;
  }
  return nullptr;
}

Result<std::size_t> DetectorSet::index_of(std::string_view name) const {
  for (std::size_t i = 0; i < configs_.size(); ++i) {
    if (configs_[i].name == name) return i;
  }
  return fail(ErrorKind::Config, "unknown detector '" + std::string(name) + "'");
}

Result<DetectorState> DetectorSet::state(std::string_view name) const {
  auto i = index_of(name);
  if (!i) return fail(i.error());
  return states_[*i];
}

std::vector<DetectorState> DetectorSet::states() const { return states_; }

std::vector<DetectorId> DetectorSet::active() const {
  std::vector<DetectorId> out;
  for (const auto& s : states_) {
    if (s.active) out.push_back(s.detector);
  }
  return out;
}

template <class Mutate>
Result<void> DetectorSet::update(std::string_view name, Mutate mutate) {
  auto i = index_of(name);
  if (!i) return fail(i.error());
  DetectorState next = states_[*i];
  mutate(next);
  next.ts = states_[*i].ts;
  if (next == states_[*i]) return {};
  if (clock_) next.ts = clock_->now();
  states_[*i] = next;
  if (listener_) listener_(next);
  return {};
}

Result<void> DetectorSet::set_active(std::string_view name, bool active) {
  return update(name, [&](DetectorState& s) { s.active = active; });
}

Result<void> DetectorSet::set_isotope(std::string_view name, std::string isotope) {
  return update(name, [&](DetectorState& s) { s.isotope = std::move(isotope); });
}

Result<void> DetectorSet::record_protected(std::string_view name, bool on) {
  return update(name, [&](DetectorState& s) { s.protected_ = on; });
}

Result<void> DetectorSet::record_deflection(std::string_view name, double value) {
  return update(name, [&](DetectorState& s) { s.deflection = value; });
}

Result<void> DetectorSet::record_gain(std::string_view name, double value) {
  return update(name, [&](DetectorState& s) { s.gain = value; });
}

Result<void> DetectorSet::record_cdd_voltage(std::string_view name, double volts) {
  return update(name, [&](DetectorState& s) { s.cdd_voltage = volts; });
}

}  // namespace pychron::spectrometer
