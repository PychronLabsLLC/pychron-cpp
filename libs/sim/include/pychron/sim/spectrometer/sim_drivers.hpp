#pragma once

// Simulated spectrometer drivers (spectrometer spec section 8.2), all bound to
// one BeamModel:
//
//   sim_integrated      all five roles, like a Qtegra/NGX/Quadera box;
//                       moving() is true for `move_time_ms` after set()
//   sim_dac_positioner  write-only DAC: read() returns the cached setpoint
//   sim_adc_bank        acquirer, raw fA frames at `sample_hz`
//   sim_pulse_counter   acquirer, raw count frames at `sample_hz`
//   sim_hv_supply       source, HV only
//
// The transport a config gives them is never touched. Registry-built drivers
// find their BeamModel through BeamModelRegistry under the `beam` option
// (default "default"); tests construct them directly with a model. Time comes
// from the model's Clock, so a ManualClock drives frame pacing and motion.

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/roles.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"

namespace pychron::sim {

namespace detail {

// Decides when the next frame of a fixed-period stream is due. wait() blocks
// on the Clock until due or until `timeout` passes on the clock or in real
// time (so a ManualClock nobody advances cannot hang a caller).
class FramePacer {
 public:
  explicit FramePacer(const Clock& clock) : clock_(clock) {}

  void start(Duration period);
  void stop();
  bool running() const;
  Duration period() const;

  // Timestamp of a due frame, or nullopt on timeout. Also assigns `seq`.
  std::optional<TimePoint> wait(Duration timeout, std::uint64_t& seq);

 private:
  const Clock& clock_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool running_ = false;
  Duration period_{};
  TimePoint due_{};
  std::uint64_t seq_ = 0;
};

// Source-role behaviour shared by the two sim sources.
class SimSourceCore {
 public:
  SimSourceCore(std::shared_ptr<BeamModel> beam, bool hv_only);

  Result<void> set_hv(double volts);
  Result<double> read_hv();
  std::span<const spectrometer::ParamSpec> params() const { return params_; }
  Result<void> set_param(const spectrometer::ParamId& id, double value);
  Result<spectrometer::Readback> read_param(const spectrometer::ParamId& id);

 private:
  std::shared_ptr<BeamModel> beam_;
  std::vector<spectrometer::ParamSpec> params_;
};

}  // namespace detail

// Legal integration periods are multiples of this.
inline constexpr Duration kSimIntegrationQuantum = std::chrono::milliseconds(100);

class SimIntegrated final : public Device,
                            public spectrometer::IMassPositioner,
                            public spectrometer::IBeamSource,
                            public spectrometer::IIntensityAcquirer,
                            public spectrometer::IDetectorControl,
                            public spectrometer::IBeamBlank {
 public:
  struct Options {
    std::vector<spectrometer::ChannelId> channels{"H2", "H1", "AX", "L1", "L2", "CDD"};
    Duration move_time = std::chrono::milliseconds(200);
    Axis axis = Axis::Dac;
    spectrometer::Limits limits{0.0, 10.0};
  };

  SimIntegrated(std::string name, std::shared_ptr<BeamModel> beam, Options options);

  static DriverSchema schema();
  static Result<std::unique_ptr<SimIntegrated>> create(const DriverArgs& args);

  // IMassPositioner
  Axis native_axis() const override { return options_.axis; }
  Result<void> set(double value) override;
  Result<double> read() override;
  Result<bool> moving() override;
  spectrometer::Limits limits() const override { return options_.limits; }

  // IBeamSource
  Result<void> set_hv(double volts) override { return source_.set_hv(volts); }
  Result<double> read_hv() override { return source_.read_hv(); }
  std::span<const spectrometer::ParamSpec> params() const override { return source_.params(); }
  Result<void> set_param(const spectrometer::ParamId& id, double value) override {
    return source_.set_param(id, value);
  }
  Result<spectrometer::Readback> read_param(const spectrometer::ParamId& id) override {
    return source_.read_param(id);
  }

  // IIntensityAcquirer: integrated frames, one per configured period.
  std::vector<spectrometer::ChannelId> channels() const override { return options_.channels; }
  bool integrates() const override { return true; }
  Result<void> configure(Duration integration) override;
  Result<void> start() override;
  Result<void> stop() override;
  Result<std::optional<spectrometer::Frame>> next(Duration timeout) override;

  // IDetectorControl
  spectrometer::Caps caps() const override;
  Result<void> protect(const spectrometer::ChannelId& channel, bool on) override;
  Result<void> set_deflection(const spectrometer::ChannelId& channel, double value) override;
  Result<double> read_deflection(const spectrometer::ChannelId& channel) override;
  Result<void> set_gain(const spectrometer::ChannelId& channel, double value) override;
  Result<double> read_gain(const spectrometer::ChannelId& channel) override;
  Result<void> set_cdd_voltage(const spectrometer::ChannelId& channel, double volts) override;

  // IBeamBlank
  Result<void> blank(bool on) override;

  // Integration period after snapping.
  Duration integration() const;

 private:
  Result<void> check_channel(const spectrometer::ChannelId& channel) const;

  std::shared_ptr<BeamModel> beam_;
  Options options_;
  detail::SimSourceCore source_;
  detail::FramePacer pacer_;
  mutable std::mutex mutex_;
  Duration integration_ = std::chrono::seconds(1);
  TimePoint moving_until_{};
};

class SimDacPositioner final : public Device, public spectrometer::IMassPositioner {
 public:
  struct Options {
    int channel = 0;
    spectrometer::Limits limits{0.0, 10.0};
  };

  SimDacPositioner(std::string name, std::shared_ptr<BeamModel> beam, Options options);

  static DriverSchema schema();
  static Result<std::unique_ptr<SimDacPositioner>> create(const DriverArgs& args);

  Axis native_axis() const override { return Axis::Dac; }
  Result<void> set(double value) override;
  Result<double> read() override;
  spectrometer::Limits limits() const override { return options_.limits; }

 private:
  std::shared_ptr<BeamModel> beam_;
  Options options_;
  mutable std::mutex mutex_;
  double cached_ = 0.0;
};

// Shared by sim_adc_bank and sim_pulse_counter: raw frames at sample_hz.
class SimRawAcquirer : public Device, public spectrometer::IIntensityAcquirer {
 public:
  std::vector<spectrometer::ChannelId> channels() const override { return channels_; }
  bool integrates() const override { return false; }
  // Raw acquirers ignore the period (the host integrates) but validate it.
  Result<void> configure(Duration integration) override;
  Result<void> start() override;
  Result<void> stop() override;
  Result<std::optional<spectrometer::Frame>> next(Duration timeout) override;

  Duration sample_period() const { return period_; }

 protected:
  SimRawAcquirer(std::string name, std::shared_ptr<BeamModel> beam, std::vector<spectrometer::ChannelId> channels,
                 double sample_hz, bool counts);

 private:
  std::shared_ptr<BeamModel> beam_;
  std::vector<spectrometer::ChannelId> channels_;
  Duration period_;
  bool counts_;  // frame values are counts per sample instead of fA
  detail::FramePacer pacer_;
};

class SimAdcBank final : public SimRawAcquirer {
 public:
  SimAdcBank(std::string name, std::shared_ptr<BeamModel> beam, std::vector<spectrometer::ChannelId> channels,
             double sample_hz = 100.0)
      : SimRawAcquirer(std::move(name), std::move(beam), std::move(channels), sample_hz, false) {}

  static DriverSchema schema();
  static Result<std::unique_ptr<SimAdcBank>> create(const DriverArgs& args);
};

class SimPulseCounter final : public SimRawAcquirer {
 public:
  SimPulseCounter(std::string name, std::shared_ptr<BeamModel> beam, std::vector<spectrometer::ChannelId> channels,
                  double sample_hz = 10.0)
      : SimRawAcquirer(std::move(name), std::move(beam), std::move(channels), sample_hz, true) {}

  static DriverSchema schema();
  static Result<std::unique_ptr<SimPulseCounter>> create(const DriverArgs& args);
};

class SimHvSupply final : public Device, public spectrometer::IBeamSource {
 public:
  SimHvSupply(std::string name, std::shared_ptr<BeamModel> beam);

  static DriverSchema schema();
  static Result<std::unique_ptr<SimHvSupply>> create(const DriverArgs& args);

  Result<void> set_hv(double volts) override { return source_.set_hv(volts); }
  Result<double> read_hv() override { return source_.read_hv(); }
  std::span<const spectrometer::ParamSpec> params() const override { return source_.params(); }
  Result<void> set_param(const spectrometer::ParamId& id, double value) override {
    return source_.set_param(id, value);
  }
  Result<spectrometer::Readback> read_param(const spectrometer::ParamId& id) override {
    return source_.read_param(id);
  }

 private:
  detail::SimSourceCore source_;
};

}  // namespace pychron::sim
