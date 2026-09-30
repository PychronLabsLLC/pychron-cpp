#pragma once

// Capability of a gauge controller that reads several channels over one
// connection (e.g. [drivers.ig] channels = [1, 2, 3] serving gauges IG1..IG3).
// A manager maps each configured gauge's `channel` to read_pressure(channel)
// without knowing the vendor.

#include <vector>

#include "pychron/core/error.hpp"

namespace pychron {

struct IChannelPressureGauge {
  virtual ~IChannelPressureGauge() = default;
  // Configured channels, in config order.
  virtual std::vector<int> pressure_channels() const = 0;
  // One reading from `channel` in the gauge's configured units. Config error
  // if `channel` is not one of pressure_channels().
  virtual Result<double> read_pressure(int channel) = 0;
};

}  // namespace pychron
