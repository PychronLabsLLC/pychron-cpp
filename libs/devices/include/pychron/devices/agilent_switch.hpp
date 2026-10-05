#pragma once

// Agilent / Keysight 34970A-family unit with a 34903A switch card as a valve
// actuator (config kind "agilent_switch"; legacy AgilentGPActuator, five
// labs). Speaks codec::agilent over a serial or TCP transport.
//
// A valve address is the channel, "101" = slot 1, channel 01. Opening a
// valve opens its relay (ROUT:OPEN) and read() asks ROUT:OPEN?, as legacy
// pychron did. `invert = true` swaps both (ROUT:CLOSE / ROUT:CLOSE?), for a
// unit whose relays energise the solenoid when closed; it is the unit's
// setting, where a valve's own `inverted` is that valve's.
//
// Route commands get no reply, so each is followed by SYST:ERR? in the same
// transport transaction: an instrument error fails the command with the
// instrument's message, and the queue is drained (at most 10 reads) so the
// next command starts clean. connect() checks *IDN? names a 34970A-family
// unit; it never sends *TST?.

#include <memory>
#include <string>

#include "pychron/codecs/agilent.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/connectable.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

class AgilentSwitch final : public Device, public IConnectable, public IValveActuator {
 public:
  static constexpr int kMaxErrorReads = 10;

  AgilentSwitch(std::string name, Transport& transport, bool invert = false, DeviceOptions options = {});

  static DriverSchema schema();
  static Result<std::unique_ptr<AgilentSwitch>> create(const DriverArgs& args);

  Result<void> connect() override;
  Result<void> open(const ValveAddress& address) override;
  Result<void> close(const ValveAddress& address) override;
  Result<ValveState> read(const ValveAddress& address) override;

  bool invert() const noexcept { return invert_; }

 private:
  Result<void> route(const ValveAddress& address, bool open_valve);
  // Reads SYST:ERR? until "+0"; the first instrument error, if any, as a
  // Protocol error naming `after`.
  Result<void> drain_errors(std::string_view after);
  Result<Bytes> ask(const codec::Command& command);

  Transport& transport_;
  bool invert_;
};

}  // namespace pychron
