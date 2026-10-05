#pragma once

// Valves switched through Thermo Qtegra RemoteControl (config kind
// "qtegra_valves"; legacy QtegraGPActuator and its lab-named copies, at
// melbourne, ldeo and usgsdenver). A valve address is the Qtegra valve name
// ("Valve 1_9 Set"), sent as is.
//
// Qtegra accepts one client, so where the spectrometer is a thermo_qtegra
// the valves share its connection: give this driver a transport of
// kind = "link", link = "<the spectrometer's link name>" (qtegra_link.hpp).
// On a tcp or udp transport of its own it owns the connection and
// registers it under its `link` option (default: the driver name).
//
// Open/Close must be answered "OK"; GetValveState "True" or "False". Any
// other reply, an ERROR among them, is a Protocol error, never a state.

#include <memory>
#include <string>

#include "pychron/codecs/thermo_qtegra.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/qtegra_link.hpp"

namespace pychron::spectrometer {

class QtegraValves final : public Device, public IValveActuator {
 public:
  QtegraValves(std::string name, QtegraLinkHandle link, DeviceOptions options = {});

  static DriverSchema schema();
  static Result<std::unique_ptr<QtegraValves>> create(const DriverArgs& args);

  Result<void> open(const ValveAddress& address) override;
  Result<void> close(const ValveAddress& address) override;
  Result<ValveState> read(const ValveAddress& address) override;

  bool owns_link() const noexcept { return link_.owner(); }

 private:
  Result<void> actuate(const ValveAddress& address, bool open);

  QtegraLinkHandle link_;
};

}  // namespace pychron::spectrometer
