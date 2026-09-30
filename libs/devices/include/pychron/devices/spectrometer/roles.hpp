#pragma once

// Spectrometer role interfaces. An integrated vendor driver (Qtegra, NGX)
// implements all five on one transport; a legacy instrument binds one driver
// per role, each on its own transport. The systems-layer Spectrometer
// composes them; managers, jobs and UI never see roles directly.
//
// Calls block until the hardware answers (or the transport times out) and are
// made from scheduler/manager threads, never the UI thread.

#include <optional>
#include <span>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/spectrometer/params.hpp"
#include "pychron/devices/spectrometer/types.hpp"

namespace pychron::spectrometer {

struct IMassPositioner {
  // What the hardware natively accepts.
  enum class Axis { Dac, Field, Mass };

  virtual ~IMassPositioner() = default;

  virtual Axis native_axis() const = 0;
  // Native units. Values outside limits() fail with Config and leave the
  // position unchanged.
  virtual Result<void> set(double value) = 0;
  // May be the cached setpoint for write-only hardware.
  virtual Result<double> read() = 0;
  virtual Result<bool> moving() { return false; }
  virtual Limits limits() const = 0;
};

struct IBeamSource {
  virtual ~IBeamSource() = default;

  virtual Result<void> set_hv(double volts) = 0;
  virtual Result<double> read_hv() = 0;
  // Supported params with their vendor names.
  virtual std::span<const ParamSpec> params() const = 0;
  // Config error for params not advertised writable by params().
  virtual Result<void> set_param(const ParamId& id, double value) = 0;
  // Config error for params not advertised readable by params().
  virtual Result<Readback> read_param(const ParamId& id) = 0;
};

struct IIntensityAcquirer {
  virtual ~IIntensityAcquirer() = default;

  virtual std::vector<ChannelId> channels() const = 0;
  // true: the vendor integrates and frames are integrated; false: raw samples
  // the host integrates.
  virtual bool integrates() const = 0;
  // Integrated mode snaps to the nearest legal value; the actual period is
  // reported as Frame::span. Non-positive durations fail with Config.
  virtual Result<void> configure(Duration integration) = 0;
  virtual Result<void> start() = 0;
  virtual Result<void> stop() = 0;
  // Starts one acquisition period on trigger-driven backends (NGX StartAcq);
  // no-op for polled backends.
  virtual Result<void> trigger() { return {}; }
  // Next frame, or nullopt if none arrived within `timeout`. Frames have
  // strictly increasing seq and non-decreasing ts.
  virtual Result<std::optional<Frame>> next(Duration timeout) = 0;
};

struct IDetectorControl {
  virtual ~IDetectorControl() = default;

  virtual Caps caps() const = 0;
  virtual Result<void> protect(const ChannelId& channel, bool on) = 0;
  // Operations outside caps() fail with Config; these defaults do exactly that.
  virtual Result<void> set_deflection(const ChannelId& channel, double value);
  virtual Result<double> read_deflection(const ChannelId& channel);
  virtual Result<void> set_gain(const ChannelId& channel, double value);
  virtual Result<double> read_gain(const ChannelId& channel);
  virtual Result<void> set_cdd_voltage(const ChannelId& channel, double volts);
};

struct IBeamBlank {
  virtual ~IBeamBlank() = default;
  virtual Result<void> blank(bool on) = 0;
};

// "dac", "field", "mass".
std::string_view to_string(IMassPositioner::Axis axis) noexcept;

// Inverse of to_string(Axis); Config error otherwise.
Result<IMassPositioner::Axis> parse_axis(std::string_view text);

// Config error "<op> not supported" for a missing detector capability.
Error unsupported(DetectorCap cap);

}  // namespace pychron::spectrometer
