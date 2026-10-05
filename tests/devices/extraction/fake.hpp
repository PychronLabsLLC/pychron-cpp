#pragma once

// In-memory extraction device and valve service. The fake exposes exactly the
// features it is constructed with, so capability queries and the conformance
// suites can be exercised without hardware.

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/devices/extraction/services.hpp"

namespace pychron::extraction::testing {

class FakeExtractionDevice final : public IExtractionDevice,
                                   public ILaserDevice,
                                   public IFurnaceDevice,
                                   public IStage,
                                   public IPatternRunner,
                                   public IPipetteService,
                                   public ICryo,
                                   public IMotorService,
                                   public IImaging {
 public:
  explicit FakeExtractionDevice(std::string name, CapabilitySet features = {})
      : name_(std::move(name)), features_(features) {}

  // IExtractionDevice
  const std::string& device_name() const override { return name_; }
  Result<void> enable() override {
    enabled_ = true;
    return {};
  }
  Result<void> disable() override {
    enabled_ = false;
    return end_extract();
  }
  Result<bool> is_enabled() override { return enabled_; }
  Result<void> extract(double value, ExtractUnits units) override {
    if (!supports(units)) return fail(not_supported(to_string(units), name_));
    if (value < 0) return fail(ErrorKind::Config, "negative output", name_);
    if (!enabled_) return fail(ErrorKind::Interlock, "disabled", name_);
    output_ = value;
    return {};
  }
  Result<void> end_extract() override {
    output_ = 0;
    firing_ = false;
    return {};
  }
  Result<double> output() override { return output_; }
  bool supports(ExtractUnits units) const override { return units != ExtractUnits::Watts; }

  ILaserDevice* laser() override { return pick<ILaserDevice>(Capability::Laser); }
  IFurnaceDevice* furnace() override { return pick<IFurnaceDevice>(Capability::Furnace); }
  IStage* stage() override { return pick<IStage>(Capability::Stage); }
  IPatternRunner* pattern_runner() override { return pick<IPatternRunner>(Capability::Pattern); }
  IPipetteService* pipettes() override { return pick<IPipetteService>(Capability::Pipette); }
  ICryo* cryo() override { return pick<ICryo>(Capability::Cryo); }
  IMotorService* motors() override { return pick<IMotorService>(Capability::Motor); }
  IImaging* imaging() override { return pick<IImaging>(Capability::Imaging); }

  // ILaserDevice
  Result<void> fire_laser() override {
    if (!enabled_) return fail(ErrorKind::Interlock, "disabled", name_);
    firing_ = true;
    return {};
  }
  Result<void> stop_laser() override {
    firing_ = false;
    return {};
  }
  Result<bool> is_firing() override { return firing_; }
  Result<void> warmup() override { return {}; }

  // IFurnaceDevice
  Result<double> read_temperature() override { return 20.0 + output_; }
  Result<void> set_pid_parameters(double) override { return {}; }
  Result<void> drop_sample(std::string_view position) override { return known(position); }
  Result<void> dump_sample() override { return {}; }

  // IStage
  Result<void> move_to_position(std::string_view position, bool) override {
    if (auto r = known(position); !r) return r;
    auto index = static_cast<double>(std::find(holes_.begin(), holes_.end(), position) - holes_.begin());
    at_ = StagePosition{index, index, at_.z};
    return {};
  }
  Result<void> set_axis(Axis axis, double value) override {
    (axis == Axis::X ? at_.x : axis == Axis::Y ? at_.y : at_.z) = value;
    return {};
  }
  Result<void> set_xy(double x, double y, double = 0) override {
    at_.x = x;
    at_.y = y;
    return {};
  }
  Result<StagePosition> position() override { return at_; }
  Result<bool> moving() override { return false; }
  Result<void> set_tray(std::string_view tray) override {
    if (tray != "221-hole") return fail(ErrorKind::Config, "unknown tray", name_);
    return {};
  }
  std::vector<std::string> positions() const override { return holes_; }

  // IPatternRunner
  Result<void> execute_pattern(std::string_view pattern) override {
    if (pattern != "spiral") return fail(ErrorKind::Config, "unknown pattern", name_);
    running_ = true;
    return {};
  }
  Result<bool> running() override { return running_; }
  Result<void> stop_pattern() override {
    running_ = false;
    return {};
  }
  std::vector<std::string> patterns() const override { return {"spiral"}; }

  // IPipetteService
  Result<void> load_pipette(std::string_view) override { return {}; }
  Result<void> extract_pipette(std::string_view) override { return {}; }
  std::vector<std::string> pipette_names() const override { return {"air"}; }

  // ICryo
  Result<void> set_cryo(double k) override {
    cryo_ = k;
    return {};
  }
  Result<double> get_cryo_temp(int) override { return cryo_; }

  // IMotorService
  Result<void> set_motor(std::string_view name, double value) override {
    motor_[std::string(name)] = value;
    return {};
  }
  Result<double> get_value(std::string_view name) override { return motor_[std::string(name)]; }
  Result<bool> motor_moving(std::string_view) override { return false; }
  std::vector<std::string> motor_names() const override { return {"attenuator"}; }

  // IImaging
  Result<std::string> snapshot(std::string_view name) override { return std::string(name) + ".jpg"; }
  Result<void> start_video_recording(std::string_view) override { return {}; }
  Result<void> stop_video_recording() override { return {}; }

 private:
  template <class I>
  I* pick(Capability c) {
    return features_.has(c) ? static_cast<I*>(this) : nullptr;
  }
  Result<void> known(std::string_view position) const {
    if (std::find(holes_.begin(), holes_.end(), position) == holes_.end())
      return fail(ErrorKind::Config, "unknown position", name_);
    return {};
  }

  std::string name_;
  CapabilitySet features_;
  bool enabled_ = false;
  bool firing_ = false;
  bool running_ = false;
  double output_ = 0;
  double cryo_ = 300;
  StagePosition at_;
  std::vector<std::string> holes_{"1", "2", "3"};
  std::map<std::string, double> motor_;
};

class FakeValveService final : public IValveService {
 public:
  explicit FakeValveService(std::vector<std::string> names) {
    for (auto& n : names) open_[n] = false;
  }
  Result<void> open(std::string_view n) override { return set(n, true); }
  Result<void> close(std::string_view n) override { return set(n, false); }
  Result<void> lock(std::string_view n) override { return known(n); }
  Result<void> unlock(std::string_view n) override { return known(n); }
  Result<bool> is_open(std::string_view n) override { return find(n); }
  Result<bool> is_closed(std::string_view n) override {
    auto o = find(n);
    if (!o) return fail(o.error());
    return !*o;
  }
  bool contains(std::string_view n) const override { return open_.contains(std::string(n)); }
  std::vector<std::string> names() const override {
    std::vector<std::string> out;
    for (auto& [n, _] : open_) out.push_back(n);
    return out;
  }

 private:
  Result<bool> find(std::string_view n) const {
    auto it = open_.find(std::string(n));
    if (it == open_.end()) return fail(ErrorKind::Config, "unknown valve " + std::string(n));
    return it->second;
  }
  Result<void> known(std::string_view n) const {
    if (!contains(n)) return fail(ErrorKind::Config, "unknown valve " + std::string(n));
    return {};
  }
  Result<void> set(std::string_view n, bool o) {
    if (auto r = known(n); !r) return r;
    open_[std::string(n)] = o;
    return {};
  }
  std::map<std::string, bool> open_;
};

}  // namespace pychron::extraction::testing
