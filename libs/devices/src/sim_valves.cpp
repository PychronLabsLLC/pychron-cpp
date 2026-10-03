#include "pychron/devices/sim_valves.hpp"

namespace pychron {

SimValves::SimValves(std::string name, DeviceOptions options) : Device(std::move(name), options) {}

DriverSchema SimValves::schema() {
  return {"", "simulated valve actuator: any address, state kept in memory; a stand-in for a controller without a driver", {}};
}

Result<std::unique_ptr<SimValves>> SimValves::create(const DriverArgs& args) {
  DeviceOptions options;
  options.clock = args.clock;
  return std::make_unique<SimValves>(args.name, options);
}

Result<void> SimValves::open(const ValveAddress& address) {
  std::lock_guard lock(mutex_);
  open_[address.value] = true;
  return observe(Result<void>{});
}

Result<void> SimValves::close(const ValveAddress& address) {
  std::lock_guard lock(mutex_);
  open_[address.value] = false;
  return observe(Result<void>{});
}

Result<ValveState> SimValves::read(const ValveAddress& address) {
  std::lock_guard lock(mutex_);
  auto it = open_.find(address.value);
  return observe(Result<ValveState>(it != open_.end() && it->second ? ValveState::Open : ValveState::Closed));
}

}  // namespace pychron

REGISTER_DRIVER("sim_valves", pychron::SimValves);
