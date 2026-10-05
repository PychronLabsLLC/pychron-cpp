#pragma once

// Line services the script host binds `open close lock unlock is_open
// is_closed` and `get_pressure get_manometer_pressure` to, and the bundle of
// handles one run's host receives.
//
// IValveService is implemented over the systems-layer SwitchManager
// (pychron/systems/switch_valve_service.hpp), so every script actuation goes
// through its locks, ownership and interlocks.

#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/devices/extraction/capability.hpp"
#include "pychron/devices/extraction/interfaces.hpp"

namespace pychron::extraction {

struct IValveService {
  virtual ~IValveService() = default;
  // Unknown names are Config errors; refusals (lock, owner, interlock) are
  // Interlock errors.
  virtual Result<void> open(std::string_view name) = 0;
  virtual Result<void> close(std::string_view name) = 0;
  virtual Result<void> lock(std::string_view name) = 0;
  virtual Result<void> unlock(std::string_view name) = 0;
  virtual Result<bool> is_open(std::string_view name) = 0;
  virtual Result<bool> is_closed(std::string_view name) = 0;
  // For the static check's valve-name resolution.
  virtual bool contains(std::string_view name) const = 0;
  virtual std::vector<std::string> names() const = 0;
};

struct IPressureService {
  virtual ~IPressureService() = default;
  // Latest reading of `gauge` on `controller`. Config error if unknown.
  virtual Result<double> get_pressure(std::string_view controller, std::string_view gauge) = 0;
  // Capacitance manometer reading (e.g. for pipette/air aliquots).
  virtual Result<double> get_manometer_pressure(std::string_view name) = 0;
};

// What one run's script host may use. Any pointer may be null; the static
// check and the host consult capabilities() before binding a command.
struct ExtractionServices {
  IExtractionDevice* device = nullptr;
  IValveService* valves = nullptr;
  IPressureService* pressure = nullptr;
  // The line's cryostat (legacy reached it through the extraction line, not
  // the extract device). When set it answers the cryo commands; otherwise
  // the device's own cryo(), if any.
  ICryo* cryo = nullptr;
};

// Device features plus Valves/Pressure for the non-null line services.
CapabilitySet capabilities(const ExtractionServices& services);

}  // namespace pychron::extraction
