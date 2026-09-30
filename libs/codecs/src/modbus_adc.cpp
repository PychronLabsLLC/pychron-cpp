#include "pychron/codecs/modbus_adc.hpp"

#include <bit>
#include <cmath>
#include <string>

namespace pychron::codec::modbus_adc {

namespace {

constexpr std::size_t kMbapSize = 7;  // tid(2) protocol(2) length(2) unit(1)

void put_u16(Bytes& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

std::uint16_t get_u16(const Bytes& b, std::size_t at) {
  return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}

// MBAP header for a PDU of `pdu_size` bytes.
Bytes mbap(std::uint16_t tid, std::uint8_t unit, std::size_t pdu_size) {
  Bytes out;
  put_u16(out, tid);
  put_u16(out, 0);
  put_u16(out, static_cast<std::uint16_t>(pdu_size + 1));
  out.push_back(unit);
  return out;
}

}  // namespace

Result<Command> read_input_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start,
                                     std::uint16_t count) {
  if (count < 1 || count > kMaxRegisters) {
    return fail(ErrorKind::Config, "modbus_adc: register count " + std::to_string(count) + " is outside 1..125");
  }
  if (static_cast<std::uint32_t>(start) + count > 0x10000U) {
    return fail(ErrorKind::Config, "modbus_adc: registers run past 0xFFFF");
  }
  Bytes tx = mbap(tid, unit, 5);
  tx.push_back(kReadInputRegisters);
  put_u16(tx, start);
  put_u16(tx, count);
  return Command{std::move(tx), ReadSpec::modbus_tcp()};
}

Result<Command> read_channels(std::uint16_t tid, std::uint8_t unit, std::uint16_t start, std::size_t channels) {
  if (channels < 1 || channels * kRegistersPerChannel > kMaxRegisters) {
    return fail(ErrorKind::Config, "modbus_adc: " + std::to_string(channels) + " channels is outside 1..62");
  }
  return read_input_registers(tid, unit, start, static_cast<std::uint16_t>(channels * kRegistersPerChannel));
}

Result<std::vector<std::uint16_t>> decode_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t count,
                                                    const Bytes& reply) {
  if (reply.size() < kMbapSize + 2) return protocol_error("modbus_adc: short frame", reply);
  if (get_u16(reply, 0) != tid) return protocol_error("modbus_adc: transaction id mismatch", reply);
  if (get_u16(reply, 2) != 0) return protocol_error("modbus_adc: not Modbus protocol", reply);
  if (get_u16(reply, 4) != reply.size() - 6) return protocol_error("modbus_adc: length mismatch", reply);
  if (reply[6] != unit) return protocol_error("modbus_adc: unit id mismatch", reply);
  const std::uint8_t function = reply[7];
  if (function == (kReadInputRegisters | kExceptionFlag)) {
    return protocol_error("modbus_adc: exception " + std::to_string(reply[8]), reply);
  }
  if (function != kReadInputRegisters) return protocol_error("modbus_adc: unexpected function", reply);
  const std::size_t bytes = reply[8];
  if (bytes != static_cast<std::size_t>(count) * 2 || reply.size() != kMbapSize + 2 + bytes) {
    return protocol_error("modbus_adc: byte count mismatch", reply);
  }
  std::vector<std::uint16_t> regs;
  regs.reserve(count);
  for (std::size_t i = 0; i < count; ++i) regs.push_back(get_u16(reply, kMbapSize + 2 + 2 * i));
  return regs;
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
  const auto bits = std::bit_cast<std::uint32_t>(value);
  return {static_cast<std::uint16_t>(bits >> 16), static_cast<std::uint16_t>(bits & 0xFFFF)};
}

float to_float(std::uint16_t high, std::uint16_t low) noexcept {
  return std::bit_cast<float>((static_cast<std::uint32_t>(high) << 16) | low);
}

Result<Request> decode_request(const Bytes& tx) {
  if (tx.size() != kMbapSize + 5) return protocol_error("modbus_adc: bad request length", tx);
  if (get_u16(tx, 2) != 0 || get_u16(tx, 4) != 6) return protocol_error("modbus_adc: bad MBAP header", tx);
  return Request{get_u16(tx, 0), tx[6], tx[7], get_u16(tx, 8), get_u16(tx, 10)};
}

Bytes encode_registers(std::uint16_t tid, std::uint8_t unit, const std::vector<std::uint16_t>& registers) {
  Bytes out = mbap(tid, unit, 2 + 2 * registers.size());
  out.push_back(kReadInputRegisters);
  out.push_back(static_cast<std::uint8_t>(2 * registers.size()));
  for (auto r : registers) put_u16(out, r);
  return out;
}

Bytes encode_exception(std::uint16_t tid, std::uint8_t unit, std::uint8_t function, std::uint8_t code) {
  Bytes out = mbap(tid, unit, 2);
  out.push_back(static_cast<std::uint8_t>(function | kExceptionFlag));
  out.push_back(code);
  return out;
}

}  // namespace pychron::codec::modbus_adc
