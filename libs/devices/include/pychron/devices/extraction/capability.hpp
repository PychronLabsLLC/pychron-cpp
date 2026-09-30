#pragma once

// Capability names shared by the extraction-device interfaces, the script
// host's static check and the run's capability query. A script command that
// needs a capability the run lacks is a NotSupported error: ErrorKind::Config
// whose message starts "not supported: " (see not_supported()).

#include <bitset>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::extraction {

enum class Capability {
  Laser,     // ILaserDevice
  Furnace,   // IFurnaceDevice
  Stage,     // IStage
  Pattern,   // IPatternRunner
  Pipette,   // IPipetteService
  Cryo,      // ICryo
  Motor,     // IMotorService
  Imaging,   // IImaging
  Pressure,  // IPressureService
  Valves,    // IValveService
};

inline constexpr std::size_t kCapabilityCount = 10;

// Stable lower-case name, e.g. "pattern".
std::string_view to_string(Capability capability) noexcept;
// Inverse of to_string(); nullopt for an unknown name.
std::optional<Capability> capability_from_string(std::string_view name) noexcept;

class CapabilitySet {
 public:
  CapabilitySet() = default;
  CapabilitySet(std::initializer_list<Capability> capabilities) {
    for (auto c : capabilities) add(c);
  }

  bool has(Capability c) const noexcept { return bits_.test(static_cast<std::size_t>(c)); }
  void add(Capability c) noexcept { bits_.set(static_cast<std::size_t>(c)); }
  void remove(Capability c) noexcept { bits_.reset(static_cast<std::size_t>(c)); }
  bool empty() const noexcept { return bits_.none(); }
  std::size_t size() const noexcept { return bits_.count(); }
  // Declaration order.
  std::vector<Capability> list() const;

  friend bool operator==(const CapabilitySet&, const CapabilitySet&) = default;

 private:
  std::bitset<kCapabilityCount> bits_;
};

// Config error "not supported: <what>".
Error not_supported(std::string_view what, std::string device = {});
Error not_supported(Capability capability, std::string device = {});
bool is_not_supported(const Error& error) noexcept;

}  // namespace pychron::extraction
