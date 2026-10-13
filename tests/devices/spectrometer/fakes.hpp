#pragma once

// Minimal in-memory role drivers: they exercise the role contracts and the
// conformance suite itself, and show both composition shapes (one integrated
// box, split legacy drivers).

#include <array>
#include <chrono>
#include <cmath>
#include <map>

#include "pychron/core/clock.hpp"
#include "pychron/devices/spectrometer/roles.hpp"

namespace pychron::spectrometer::fakes {

// Write-only DAC: read() returns the cached setpoint.
class FakeDac final : public IMassPositioner {
 public:
  Axis native_axis() const override { return Axis::Dac; }
  Result<void> set(double value) override {
    if (!limits().contains(value)) return fail(ErrorKind::Config, "dac out of range");
    value_ = value;
    return {};
  }
  Result<double> read() override { return value_; }
  Limits limits() const override { return {0.0, 10.0}; }

 private:
  double value_ = 0.0;
};

// Field-axis magnet that reports motion for `settle_polls` moving() calls.
class FakeMagnet final : public IMassPositioner {
 public:
  explicit FakeMagnet(int settle_polls = 3) : settle_polls_(settle_polls) {}
  Axis native_axis() const override { return Axis::Field; }
  Result<void> set(double value) override {
    if (!limits().contains(value)) return fail(ErrorKind::Config, "field out of range");
    target_ = value;
    remaining_ = settle_polls_;
    return {};
  }
  Result<double> read() override { return remaining_ > 0 ? (target_ + position_) / 2 : target_; }
  Result<bool> moving() override {
    // NOLINTNEXTLINE(bugprone-inc-dec-in-conditions): counts down to the arrival
    if (remaining_ > 0 && --remaining_ == 0) position_ = target_;
    return remaining_ > 0;
  }
  Limits limits() const override { return {-1.0, 1.0}; }

 private:
  int settle_polls_;
  int remaining_ = 0;
  double target_ = 0.0;
  double position_ = 0.0;
};

// Raw or integrated acquirer driven by a ManualClock: one frame per next()
// once `period` has elapsed since the last frame.
class FakeAcquirer final : public IIntensityAcquirer {
 public:
  FakeAcquirer(ManualClock& clock, bool integrated, std::vector<ChannelId> channels)
      : clock_(clock), integrated_(integrated), channels_(std::move(channels)) {}

  std::vector<ChannelId> channels() const override { return channels_; }
  bool integrates() const override { return integrated_; }
  Result<void> configure(Duration integration) override {
    if (integration <= Duration::zero()) return fail(ErrorKind::Config, "integration <= 0");
    if (integrated_) {
      // Thermo-style binary series: snap up to 2^k * 0.131072 s.
      Duration legal = std::chrono::microseconds(131072);
      while (legal < integration) legal *= 2;
      period_ = legal;
    } else {
      period_ = std::chrono::milliseconds(10);  // sample period
    }
    return {};
  }
  Result<void> start() override {
    running_ = true;
    last_ = clock_.now();
    return {};
  }
  Result<void> stop() override {
    running_ = false;
    return {};
  }
  Result<std::optional<Frame>> next(Duration) override {
    if (!running_ || clock_.now() - last_ < period_) return std::optional<Frame>{};
    last_ = clock_.now();
    Frame f;
    f.ts = last_;
    f.seq = ++seq_;
    f.integrated = integrated_;
    f.span = period_;
    for (std::size_t i = 0; i < channels_.size(); ++i) {
      f.values.emplace_back(channels_[i], static_cast<double>(i) + 0.5);
    }
    return std::optional<Frame>{std::move(f)};
  }

  Duration period() const { return period_; }

 private:
  ManualClock& clock_;
  bool integrated_;
  std::vector<ChannelId> channels_;
  Duration period_ = std::chrono::seconds(1);
  TimePoint last_{};
  std::uint64_t seq_ = 0;
  bool running_ = false;
};

// Source with a table of params. Setpoints of readable params read back with
// an `actual` 0.1% below.
class FakeSource final : public IBeamSource {
 public:
  explicit FakeSource(std::vector<ParamSpec> specs) : specs_(std::move(specs)) {}

  // Legacy HV-only supply.
  static FakeSource hv_only() {
    return FakeSource({{ParamId{SourceParam::HV}, Unit::Volts, {0.0, 10000.0}, true, true, "HV"}});
  }
  // Integrated vendor box with a mix of params, including a vendor extra.
  static FakeSource vendor() {
    return FakeSource({
        {ParamId{SourceParam::HV}, Unit::Volts, {0.0, 10000.0}, true, true, "HV Set"},
        {ParamId{SourceParam::TrapCurrent}, Unit::MicroAmps, {0.0, 1000.0}, true, true, "Trap Set"},
        {ParamId{SourceParam::YSymmetry}, Unit::Volts, {-50.0, 50.0}, true, true, "Y-Symmetry Set"},
        {ParamId{SourceParam::Emission}, Unit::MicroAmps, {0.0, 1000.0}, true, false, "Emission"},
        {ParamId{SourceParam::ZFocus}, Unit::Volts, {0.0, 100.0}, false, true, "Z-Focus Set"},
        {ParamId{Custom{"Aux Lens"}}, Unit::Volts, {0.0, 10.0}, true, true, "Aux Lens Set"},
    });
  }

  Result<void> set_hv(double volts) override { return set_param(ParamId{SourceParam::HV}, volts); }
  Result<double> read_hv() override {
    auto rb = read_param(ParamId{SourceParam::HV});
    if (!rb) return fail(rb.error());
    return rb->setpoint;
  }
  std::span<const ParamSpec> params() const override { return specs_; }
  Result<void> set_param(const ParamId& id, double value) override {
    const auto* spec = find_spec(specs_, id);
    if (!spec || !spec->writable) return fail(ErrorKind::Config, to_string(id) + " not writable");
    values_[to_string(id)] = value;
    return {};
  }
  Result<Readback> read_param(const ParamId& id) override {
    const auto* spec = find_spec(specs_, id);
    if (!spec || !spec->readable) return fail(ErrorKind::Config, to_string(id) + " not readable");
    const double v = values_[to_string(id)];
    return Readback{v, v * 0.999};
  }

 private:
  std::vector<ParamSpec> specs_;
  std::map<std::string, double> values_;
};

class FakeDetectorControl final : public IDetectorControl {
 public:
  explicit FakeDetectorControl(Caps caps) : caps_(caps) {}

  Caps caps() const override { return caps_; }
  Result<void> protect(const ChannelId& channel, bool on) override {
    if (!caps_.has(DetectorCap::Protect)) return fail(unsupported(DetectorCap::Protect));
    protected_[channel] = on;
    return {};
  }
  Result<void> set_deflection(const ChannelId& channel, double value) override {
    if (!caps_.has(DetectorCap::Deflection)) return IDetectorControl::set_deflection(channel, value);
    deflection_[channel] = value;
    return {};
  }
  Result<double> read_deflection(const ChannelId& channel) override {
    if (!caps_.has(DetectorCap::Deflection)) return IDetectorControl::read_deflection(channel);
    return deflection_[channel];
  }
  bool is_protected(const ChannelId& channel) { return protected_[channel]; }

 private:
  Caps caps_;
  std::map<ChannelId, bool> protected_;
  std::map<ChannelId, double> deflection_;
};

}  // namespace pychron::spectrometer::fakes
