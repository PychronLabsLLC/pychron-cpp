#pragma once

// A Modbus TCP device for SimSystem and driver tests, answering through
// codec::modbus at one unit id. What its coils and registers hold is the
// owner's: each function asks a callback, and an address the callback has
// nothing for (nullopt / false) is an "illegal data address" exception, as
// a PLC answers. Requests for another unit get no reply. Functions it has
// no callback for are "illegal function".
//
// Callbacks run on the transport's worker thread.

#include <cstdint>
#include <functional>
#include <optional>

#include "pychron/transport/sim_transport.hpp"

namespace pychron {

struct ModbusDeviceSim {
  std::uint8_t unit = 1;
  std::function<std::optional<bool>(std::uint16_t address)> read_coil;
  std::function<std::optional<std::uint16_t>(std::uint16_t address)> read_holding;
  std::function<std::optional<std::uint16_t>(std::uint16_t address)> read_input;
  // True when the address takes a write.
  std::function<bool(std::uint16_t address, bool on)> write_coil;
  std::function<bool(std::uint16_t address, std::uint16_t value)> write_register;

  Bytes respond(const Bytes& tx) const;
  // respond() as a hook; copies this, so set the callbacks first.
  SimTransport::Hook hook() const;
};

}  // namespace pychron
