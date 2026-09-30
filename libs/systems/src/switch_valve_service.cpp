#include "pychron/systems/switch_valve_service.hpp"

namespace pychron::systems {

SwitchValveService::SwitchValveService(SwitchManager& manager, std::string actor)
    : manager_(manager), actor_(std::move(actor)) {}

Result<void> SwitchValveService::open(std::string_view name) {
  return manager_.actuate(name, SwitchOp::Open, actor_);
}

Result<void> SwitchValveService::close(std::string_view name) {
  return manager_.actuate(name, SwitchOp::Close, actor_);
}

Result<void> SwitchValveService::lock(std::string_view name) { return manager_.lock(name); }

Result<void> SwitchValveService::unlock(std::string_view name) { return manager_.unlock(name); }

Result<bool> SwitchValveService::is_open(std::string_view name) {
  auto state = manager_.state(name);
  if (!state) return fail(state.error());
  return *state == ValveState::Open;
}

Result<bool> SwitchValveService::is_closed(std::string_view name) {
  auto state = manager_.state(name);
  if (!state) return fail(state.error());
  return *state == ValveState::Closed;
}

bool SwitchValveService::contains(std::string_view name) const { return manager_.contains(name); }

std::vector<std::string> SwitchValveService::names() const {
  std::vector<std::string> out;
  for (auto& info : manager_.list()) out.push_back(std::move(info.name));
  return out;
}

}  // namespace pychron::systems
