#pragma once

// SwitchValveService: the script host's IValveService over a SwitchManager.
//
// Every open/close is SwitchManager::actuate under one actor (e.g.
// "script:<run id>"), so scripts are subject to the same software locks,
// ownership and interlocks as every other caller. lock/unlock are the
// manager's software locks. is_open/is_closed report the manager's recorded
// state: an Unknown switch is neither open nor closed. names() lists valves,
// manual valves and switches in config order.

#include <string>
#include <string_view>
#include <vector>

#include "pychron/devices/extraction/services.hpp"
#include "pychron/systems/switch_manager.hpp"

namespace pychron::systems {

class SwitchValveService final : public extraction::IValveService {
 public:
  // `manager` must outlive the service.
  SwitchValveService(SwitchManager& manager, std::string actor);

  const std::string& actor() const noexcept { return actor_; }

  Result<void> open(std::string_view name) override;
  Result<void> close(std::string_view name) override;
  Result<void> lock(std::string_view name) override;
  Result<void> unlock(std::string_view name) override;
  Result<bool> is_open(std::string_view name) override;
  Result<bool> is_closed(std::string_view name) override;
  bool contains(std::string_view name) const override;
  std::vector<std::string> names() const override;

 private:
  SwitchManager& manager_;
  std::string actor_;
};

}  // namespace pychron::systems
