#pragma once

// A simulated valve actuator (config kind "sim_valves"): any address, the
// state kept in memory, every valve closed at start. It stands in for a
// controller pychron-cpp has no driver for yet, e.g. a line converted from a
// legacy Pychron setup (setup::import_legacy_line), so the line loads and
// runs in simulation. Its transport is never used.

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"

namespace pychron {

class SimValves final : public Device, public IValveActuator {
 public:
  explicit SimValves(std::string name, DeviceOptions options = {});

  static DriverSchema schema();
  static Result<std::unique_ptr<SimValves>> create(const DriverArgs& args);

  Result<void> open(const ValveAddress& address) override;
  Result<void> close(const ValveAddress& address) override;
  Result<ValveState> read(const ValveAddress& address) override;

 private:
  std::mutex mutex_;
  std::map<std::string, bool> open_;  // by address
};

}  // namespace pychron
