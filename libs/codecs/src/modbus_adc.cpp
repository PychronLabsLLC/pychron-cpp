#include "pychron/codecs/modbus_adc.hpp"

#include <cmath>
#include <string>

#include "pychron/codecs/modbus.hpp"

namespace pychron::codec::modbus_adc {

Result<Command> read_input_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start,
                                     std::uint16_t count) {
  return modbus::read_input_registers(tid, unit, start, count);
}

Result<Command> read_channels(std::uint16_t tid, std::uint8_t unit, std::uint16_t start, std::size_t channels) {
  if (channels < 1 || channels * kRegistersPerChannel > kMaxRegisters) {
    return fail(ErrorKind::Config, "modbus_adc: " + std::to_string(channels) + " channels is outside 1..62");
  }
  return read_input_registers(tid, unit, start, static_cast<std::uint16_t>(channels * kRegistersPerChannel));
}

Result<std::vector<std::uint16_t>> decode_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t count,
                                                    const Bytes& reply) {
  return modbus::decode_registers(tid, unit, modbus::Function::ReadInputRegisters, count, reply);
}

Result<std::vector<double>> decode_channels(std::uint16_t tid, std::uint8_t unit, std::size_t channels,
                                            const Bytes& reply) {
  auto regs = decode_registers(tid, unit, static_cast<std::uint16_t>(channels * kRegistersPerChannel), reply);
  if (!regs) return fail(std::move(regs).error());
  std::vector<double> values;
  values.reserve(channels);
  for (std::size_t i = 0; i < channels; ++i) {
    const double v = to_float((*regs)[2 * i], (*regs)[2 * i + 1]);
    if (!std::isfinite(v)) return protocol_error("modbus_adc: channel " + std::to_string(i) + " not finite", reply);
    values.push_back(v);
  }
  return values;
}

std::vector<std::uint16_t> to_registers(float value) {
  const auto words = modbus::encode_float(value, modbus::WordOrder::ABCD);
  return {words[0], words[1]};
}

float to_float(std::uint16_t high, std::uint16_t low) noexcept {
  return modbus::decode_float(high, low, modbus::WordOrder::ABCD);
}

Result<Request> decode_request(const Bytes& tx) {
  auto r = modbus::decode_request(tx);
  if (!r) return fail(std::move(r).error());
  if (r->function != kReadInputRegisters) return protocol_error("modbus_adc: not a read of input registers", tx);
  return Request{r->tid, r->unit, r->function, r->start, r->count};
}

Bytes encode_registers(std::uint16_t tid, std::uint8_t unit, const std::vector<std::uint16_t>& registers) {
  return modbus::encode_registers(tid, unit, modbus::Function::ReadInputRegisters, registers);
}

Bytes encode_exception(std::uint16_t tid, std::uint8_t unit, std::uint8_t function, std::uint8_t code) {
  return modbus::encode_exception(tid, unit, function, code);
}

}  // namespace pychron::codec::modbus_adc
