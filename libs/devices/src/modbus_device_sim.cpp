#include "pychron/devices/modbus_device_sim.hpp"

#include <algorithm>
#include <map>
#include <memory>
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

namespace {

// The callbacks of `parts`, asked in order.
ModbusDeviceSim chained(std::uint8_t unit, std::vector<ModbusDeviceSim> parts) {
  ModbusDeviceSim d;
  d.unit = unit;
  auto shared = std::make_shared<const std::vector<ModbusDeviceSim>>(std::move(parts));
  auto any = [&](auto member) {
    return std::any_of(shared->begin(), shared->end(), [&](const ModbusDeviceSim& p) { return bool(p.*member); });
  };
  auto read = [shared](auto member) {
    return [shared, member](std::uint16_t a) -> decltype(((*shared)[0].*member)(a)) {
      for (const auto& p : *shared) {
        if (!(p.*member)) continue;
        if (auto v = (p.*member)(a)) return v;
      }
      return std::nullopt;
    };
  };
  if (any(&ModbusDeviceSim::read_coil)) d.read_coil = read(&ModbusDeviceSim::read_coil);
  if (any(&ModbusDeviceSim::read_holding)) d.read_holding = read(&ModbusDeviceSim::read_holding);
  if (any(&ModbusDeviceSim::read_input)) d.read_input = read(&ModbusDeviceSim::read_input);
  if (any(&ModbusDeviceSim::write_coil)) {
    d.write_coil = [shared](std::uint16_t a, bool on) {
      for (const auto& p : *shared)
        if (p.write_coil && p.write_coil(a, on)) return true;
      return false;
    };
  }
  if (any(&ModbusDeviceSim::write_register)) {
    d.write_register = [shared](std::uint16_t a, std::uint16_t v) {
      for (const auto& p : *shared)
        if (p.write_register && p.write_register(a, v)) return true;
      return false;
    };
  }
  return d;
}

}  // namespace

SimTransport::Hook modbus_bus_hook(std::vector<ModbusDeviceSim> devices) {
  std::map<std::uint8_t, std::vector<ModbusDeviceSim>> by_unit;
  for (auto& d : devices) by_unit[d.unit].push_back(std::move(d));
  std::vector<ModbusDeviceSim> units;
  for (auto& [unit, parts] : by_unit) units.push_back(chained(unit, std::move(parts)));
  return [units = std::move(units)](const Bytes& tx) {
    for (const auto& u : units)
      if (Bytes reply = u.respond(tx); !reply.empty()) return reply;
    return Bytes{};
  };
}

}  // namespace pychron
