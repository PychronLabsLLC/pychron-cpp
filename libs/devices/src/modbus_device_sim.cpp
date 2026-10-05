#include "pychron/devices/modbus_device_sim.hpp"

#include <vector>

#include "pychron/codecs/modbus.hpp"

namespace pychron {

namespace mb = codec::modbus;

namespace {

constexpr std::uint8_t kIllegalFunction = 1;
constexpr std::uint8_t kIllegalAddress = 2;

}  // namespace

Bytes ModbusDeviceSim::respond(const Bytes& tx) const {
  auto request = mb::decode_request(tx);
  if (!request) {
    // A well-framed request for a function we do not read: say so.
    if (auto fn = mb::request_function(tx); fn && tx.size() >= 7 && tx[6] == unit) {
      const std::uint16_t tid = static_cast<std::uint16_t>((tx[0] << 8) | tx[1]);
      return mb::encode_exception(tid, unit, *fn, kIllegalFunction);
    }
    return {};
  }
  const auto& r = *request;
  if (r.unit != unit) return {};
  auto exception = [&](std::uint8_t code) { return mb::encode_exception(r.tid, r.unit, r.function, code); };

  switch (static_cast<mb::Function>(r.function)) {
    case mb::Function::ReadCoils: {
      if (!read_coil) return exception(kIllegalFunction);
      std::vector<bool> coils;
      for (std::uint32_t a = r.start; a < static_cast<std::uint32_t>(r.start) + r.count; ++a) {
        auto v = read_coil(static_cast<std::uint16_t>(a));
        if (!v) return exception(kIllegalAddress);
        coils.push_back(*v);
      }
      return mb::encode_coils(r.tid, r.unit, coils);
    }
    case mb::Function::ReadHoldingRegisters:
    case mb::Function::ReadInputRegisters: {
      const bool holding = r.function == static_cast<std::uint8_t>(mb::Function::ReadHoldingRegisters);
      const auto& source = holding ? read_holding : read_input;
      if (!source) return exception(kIllegalFunction);
      std::vector<std::uint16_t> regs;
      for (std::uint32_t a = r.start; a < static_cast<std::uint32_t>(r.start) + r.count; ++a) {
        auto v = source(static_cast<std::uint16_t>(a));
        if (!v) return exception(kIllegalAddress);
        regs.push_back(*v);
      }
      return mb::encode_registers(r.tid, r.unit, static_cast<mb::Function>(r.function), regs);
    }
    case mb::Function::WriteSingleCoil:
      if (!write_coil) return exception(kIllegalFunction);
      if (!write_coil(r.start, r.coil)) return exception(kIllegalAddress);
      return mb::encode_write_single_coil(r.tid, r.unit, r.start, r.coil);
    case mb::Function::WriteMultipleRegisters:
      if (!write_register) return exception(kIllegalFunction);
      for (std::size_t i = 0; i < r.values.size(); ++i) {
        if (!write_register(static_cast<std::uint16_t>(r.start + i), r.values[i])) return exception(kIllegalAddress);
      }
      return mb::encode_write_multiple_registers(r.tid, r.unit, r.start, r.count);
  }
  return exception(kIllegalFunction);
}

SimTransport::Hook ModbusDeviceSim::hook() const {
  return [self = *this](const Bytes& tx) { return self.respond(tx); };
}

}  // namespace pychron
