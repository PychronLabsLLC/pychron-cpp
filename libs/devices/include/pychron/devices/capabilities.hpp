#pragma once

// Capability interfaces. A driver composes Transport& + codec and implements
// one or more of these; managers depend on these only and are vendor-blind.
// Calls block until the device answers (or the transport times out) and are
// made from scheduler/manager threads, never the UI thread.

#include "pychron/core/error.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/types.hpp"

namespace pychron {

struct IPressureGauge {
  virtual ~IPressureGauge() = default;
  // One reading in the gauge's configured units.
  virtual Result<double> read_pressure() = 0;
};

struct IValveActuator {
  virtual ~IValveActuator() = default;
  virtual Result<void> open(const ValveAddress& address) = 0;
  virtual Result<void> close(const ValveAddress& address) = 0;
  // State as reported by the hardware (read-back), not as last commanded.
  virtual Result<ValveState> read(const ValveAddress& address) = 0;
};

struct IScannable {
  virtual ~IScannable() = default;
  // One periodic sample for the Scheduler; `device` is the device's name.
  virtual Result<Sample> sample() = 0;
};

// The capability `device` implements, or nullptr.
template <class Capability>
Capability* capability(Device& device) noexcept {
  return dynamic_cast<Capability*>(&device);
}

}  // namespace pychron
