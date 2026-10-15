#pragma once

// NCD ProXR relay board as a valve actuator (config kind "proxr_relay").
//
// A valve address is a flat relay index 0..255 (`address = "9"` is relay 1
// of bank 2). Every operation selects the bank, then sends the relay
// command, so the board's selected bank is never assumed. Energized means
// open. read() asks the board, it does not echo the last command.

#include <memory>
#include <string>

#include "pychron/codecs/proxr.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

// May each command be sent twice (Repeat, link.hpp)? Each operation is one
// transaction (bank select, then the relay command), repeated whole.
//
//   Safe    bank select, relay on, relay off, relay state read
//   Unsafe  none
class ProxrRelay final : public Device, public IValveActuator {
 public:
  ProxrRelay(std::string name, Transport& transport, DeviceOptions options = {});

  static DriverSchema schema();
  static Result<std::unique_ptr<ProxrRelay>> create(const DriverArgs& args);

  Result<void> open(const ValveAddress& address) override;
  Result<void> close(const ValveAddress& address) override;
  Result<ValveState> read(const ValveAddress& address) override;

 private:
  Result<void> actuate(const ValveAddress& address, bool energize);
  Result<ValveState> query(const ValveAddress& address);
  // Bank select + ack for the relay at `address`; its relay within the bank.
  Result<int> select(const ValveAddress& address, Resend resend);
  Result<Bytes> send(const codec::Command& command, Resend resend);

  // Bank select and relay command run as one Transport transaction so no
  // other traffic on the bus (from this or any other device) splits them.
  Transport& transport_;
};

}  // namespace pychron
