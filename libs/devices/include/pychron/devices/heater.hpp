#pragma once

// Capability of a heater the extraction line switches and regulates (plan
// 2026-10-05, E1; legacy HeaterManager). Values are in the heater's own
// units, whatever its controller's program uses; nothing converts them.
//
// A driver without one of these functions (a PLC with no use_pid coil, say)
// answers not_supported() for it (pychron/devices/extraction/capability.hpp),
// never a silent no-op.

#include "pychron/core/error.hpp"

namespace pychron {

struct IHeater {
  virtual ~IHeater() = default;
  virtual Result<void> set_enabled(bool on) = 0;
  virtual Result<bool> enabled() = 0;
  virtual Result<void> set_setpoint(double value) = 0;
  virtual Result<double> setpoint() = 0;
  virtual Result<double> readback() = 0;
  virtual Result<void> set_use_pid(bool on) = 0;
  virtual Result<bool> use_pid() = 0;
};

}  // namespace pychron
