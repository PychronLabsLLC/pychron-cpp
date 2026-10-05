#pragma once

// Valves switched by another Pychron's valve service (config kind
// "pychron_valves"; legacy PychronGPActuator: jan driving felix's valves).
// A valve address is the remote Pychron's valve name. Speaks
// codec::pychron_tx over TCP (legacy port 1061).
//
// The legacy server answers one command per connection and then closes it,
// so every command here reopens the transport, sends, reads to the close and
// closes again, all in one transport transaction.
//
// read() asks GetValveState: the state the remote Pychron holds, which it
// verified when it actuated. The remote's own refusals keep their meaning: a
// software or system lock there is an Interlock error here, an unknown valve
// a Config error.

#include <memory>
#include <string>

#include "pychron/codecs/pychron_tx.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

class PychronValves final : public Device, public IValveActuator {
 public:
  PychronValves(std::string name, Transport& transport, DeviceOptions options = {});

  static DriverSchema schema();
  static Result<std::unique_ptr<PychronValves>> create(const DriverArgs& args);

  Result<void> open(const ValveAddress& address) override;
  Result<void> close(const ValveAddress& address) override;
  Result<ValveState> read(const ValveAddress& address) override;

 private:
  // Connect, send, read to the close, disconnect.
  Result<Bytes> ask(const codec::Command& command);

  Transport& transport_;
};

}  // namespace pychron
