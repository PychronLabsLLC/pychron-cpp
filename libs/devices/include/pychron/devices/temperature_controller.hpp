#pragma once

// Capability of a temperature controller with sensor inputs and control
// loops (outputs): a cryostat's (plan 2026-10-05, C1). Kelvin at the
// interface; a controller configured for Celsius converts in its driver.

#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron {

struct ITemperatureController {
  virtual ~ITemperatureController() = default;
  // Configured inputs, in order ("A", "B").
  virtual std::vector<std::string> inputs() const = 0;
  // One reading. Config error for an input not in inputs().
  virtual Result<double> read_temperature(std::string_view input) = 0;
  // Control loops, 1..n.
  virtual int outputs() const = 0;
  // Sets the setpoint (choosing the heater range it needs) and confirms it
  // reads back. Config error for an unknown output or a value no range covers.
  virtual Result<void> set_setpoint(int output, double kelvin) = 0;
  virtual Result<double> setpoint(int output) = 0;
};

}  // namespace pychron
