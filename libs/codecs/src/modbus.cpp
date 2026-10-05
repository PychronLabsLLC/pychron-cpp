#include "pychron/codecs/modbus.hpp"

#include <algorithm>
#include <bit>
#include <cctype>

namespace pychron::codec::modbus {

namespace {

constexpr std::size_t kMbapSize = 7;  // tid(2) protocol(2) length(2) unit(1)

void put_u16(Bytes& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

std::uint16_t get_u16(const Bytes& b, std::size_t at) {
  return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}

std::uint8_t code(Function f) { return static_cast<std::uint8_t>(f); }

// A PDU in its MBAP frame.
Bytes frame(std::uint16_t tid, std::uint8_t unit, const Bytes& pdu) {
  Bytes out;
  out.reserve(kMbapSize + pdu.size());
  put_u16(out, tid);
  put_u16(out, 0);
  put_u16(out, static_cast<std::uint16_t>(pdu.size() + 1));
  out.push_back(unit);
  out.insert(out.end(), pdu.begin(), pdu.end());
  return out;
}

Result<void> check_range(std::string_view what, std::uint16_t start, std::uint16_t count, std::uint16_t max) {
  if (count < 1 || count > max) {
    return fail(ErrorKind::Config, "modbus: " + std::string(what) + " count " + std::to_string(count) +
                                       " is outside 1.." + std::to_string(max));
  }
  if (static_cast<std::uint32_t>(start) + count > 0x10000U) {
    return fail(ErrorKind::Config, "modbus: " + std::string(what) + " run past address 0xFFFF");
  }
  return {};
}

Result<Command> read_request(Function f, std::string_view what, std::uint16_t tid, std::uint8_t unit,
                             std::uint16_t start, std::uint16_t count, std::uint16_t max) {
  if (auto ok = check_range(what, start, count, max); !ok) return fail(std::move(ok).error());
  Bytes pdu{code(f)};
  put_u16(pdu, start);
  put_u16(pdu, count);
  return Command{frame(tid, unit, pdu), ReadSpec::modbus_tcp()};
}

// The reply's PDU (function code onwards) once its header matches the
// request; an exception reply for `f` is reported with its code.
Result<Bytes> reply_pdu(std::uint16_t tid, std::uint8_t unit, Function f, const Bytes& reply) {
  if (reply.size() < kMbapSize + 2) return protocol_error("modbus: short frame", reply);
  if (get_u16(reply, 0) != tid) return protocol_error("modbus: transaction id mismatch", reply);
  if (get_u16(reply, 2) != 0) return protocol_error("modbus: not Modbus protocol", reply);
  if (get_u16(reply, 4) != reply.size() - 6) return protocol_error("modbus: length mismatch", reply);
  if (reply[6] != unit) return protocol_error("modbus: unit id mismatch", reply);
  const std::uint8_t fn = reply[kMbapSize];
  if (fn == (code(f) | kExceptionFlag)) {
    if (reply.size() != kMbapSize + 2) return protocol_error("modbus: malformed exception", reply);
    const std::uint8_t c = reply[kMbapSize + 1];
    return protocol_error("modbus: exception " + std::to_string(c) + " (" + exception_name(c) + ")", reply);
  }
  if (fn != code(f)) return protocol_error("modbus: unexpected function", reply);
  return Bytes(reply.begin() + kMbapSize, reply.end());
}

std::uint32_t bits_of(std::uint16_t first, std::uint16_t second, WordOrder order) noexcept {
  auto swap_bytes = [](std::uint16_t w) { return static_cast<std::uint16_t>((w << 8) | (w >> 8)); };
  switch (order) {
    case WordOrder::ABCD:
      return (static_cast<std::uint32_t>(first) << 16) | second;
    case WordOrder::CDAB:
      return (static_cast<std::uint32_t>(second) << 16) | first;
    case WordOrder::BADC:
      return (static_cast<std::uint32_t>(swap_bytes(first)) << 16) | swap_bytes(second);
    case WordOrder::DCBA:
      return (static_cast<std::uint32_t>(swap_bytes(second)) << 16) | swap_bytes(first);
  }
  return 0;
}

std::array<std::uint16_t, 2> words_of(std::uint32_t bits, WordOrder order) noexcept {
  auto swap_bytes = [](std::uint16_t w) { return static_cast<std::uint16_t>((w << 8) | (w >> 8)); };
  const auto high = static_cast<std::uint16_t>(bits >> 16);
  const auto low = static_cast<std::uint16_t>(bits & 0xFFFF);
  switch (order) {
    case WordOrder::ABCD:
      return {high, low};
    case WordOrder::CDAB:
      return {low, high};
    case WordOrder::BADC:
      return {swap_bytes(high), swap_bytes(low)};
    case WordOrder::DCBA:
      return {swap_bytes(low), swap_bytes(high)};
  }
  return {0, 0};
}

}  // namespace

std::string exception_name(std::uint8_t c) {
  switch (c) {
    case 1:
      return "illegal function";
    case 2:
      return "illegal data address";
    case 3:
      return "illegal data value";
    case 4:
      return "device failure";
    case 5:
      return "acknowledge (still working)";
    case 6:
      return "device busy";
    case 10:
      return "gateway path unavailable";
    case 11:
      return "gateway target did not respond";
    default:
      return "unlisted exception";
  }
}

Result<Command> read_coils(std::uint16_t tid, std::uint8_t unit, std::uint16_t start, std::uint16_t count) {
  return read_request(Function::ReadCoils, "coil", tid, unit, start, count, kMaxReadCoils);
}

Result<Command> read_holding_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start,
                                       std::uint16_t count) {
  return read_request(Function::ReadHoldingRegisters, "register", tid, unit, start, count, kMaxReadRegisters);
}

Result<Command> read_input_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start, std::uint16_t count) {
  return read_request(Function::ReadInputRegisters, "register", tid, unit, start, count, kMaxReadRegisters);
}

Command write_single_coil(std::uint16_t tid, std::uint8_t unit, std::uint16_t address, bool on) {
  Bytes pdu{code(Function::WriteSingleCoil)};
  put_u16(pdu, address);
  put_u16(pdu, on ? 0xFF00 : 0x0000);
  return Command{frame(tid, unit, pdu), ReadSpec::modbus_tcp()};
}

Result<Command> write_multiple_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start,
                                         const std::vector<std::uint16_t>& values) {
  const auto count = static_cast<std::uint16_t>(std::min<std::size_t>(values.size(), 0xFFFF));
  if (values.size() > kMaxWriteRegisters || values.empty()) {
    return fail(ErrorKind::Config, "modbus: register count " + std::to_string(values.size()) +
                                       " is outside 1.." + std::to_string(kMaxWriteRegisters));
  }
  if (auto ok = check_range("register", start, count, kMaxWriteRegisters); !ok) return fail(std::move(ok).error());
  Bytes pdu{code(Function::WriteMultipleRegisters)};
  put_u16(pdu, start);
  put_u16(pdu, count);
  pdu.push_back(static_cast<std::uint8_t>(2 * count));
  for (auto v : values) put_u16(pdu, v);
  return Command{frame(tid, unit, pdu), ReadSpec::modbus_tcp()};
}

Result<std::vector<bool>> decode_coils(std::uint16_t tid, std::uint8_t unit, std::uint16_t count, const Bytes& reply) {
  auto pdu = reply_pdu(tid, unit, Function::ReadCoils, reply);
  if (!pdu) return fail(std::move(pdu).error());
  const std::size_t bytes = (static_cast<std::size_t>(count) + 7) / 8;
  if (pdu->size() < 2 || (*pdu)[1] != bytes || pdu->size() != 2 + bytes) {
    return protocol_error("modbus: byte count mismatch", reply);
  }
  std::vector<bool> coils;
  coils.reserve(count);
  for (std::size_t i = 0; i < count; ++i) coils.push_back((((*pdu)[2 + i / 8] >> (i % 8)) & 1) != 0);
  return coils;
}

Result<std::vector<std::uint16_t>> decode_registers(std::uint16_t tid, std::uint8_t unit, Function function,
                                                    std::uint16_t count, const Bytes& reply) {
  if (function != Function::ReadHoldingRegisters && function != Function::ReadInputRegisters) {
    return fail(ErrorKind::Config, "modbus: decode_registers is for functions 03 and 04");
  }
  auto pdu = reply_pdu(tid, unit, function, reply);
  if (!pdu) return fail(std::move(pdu).error());
  const std::size_t bytes = static_cast<std::size_t>(count) * 2;
  if (pdu->size() < 2 || (*pdu)[1] != bytes || pdu->size() != 2 + bytes) {
    return protocol_error("modbus: byte count mismatch", reply);
  }
  std::vector<std::uint16_t> regs;
  regs.reserve(count);
  for (std::size_t i = 0; i < count; ++i) regs.push_back(get_u16(*pdu, 2 + 2 * i));
  return regs;
}

Result<void> decode_write_single_coil(std::uint16_t tid, std::uint8_t unit, std::uint16_t address, bool on,
                                      const Bytes& reply) {
  auto pdu = reply_pdu(tid, unit, Function::WriteSingleCoil, reply);
  if (!pdu) return fail(std::move(pdu).error());
  if (pdu->size() != 5 || get_u16(*pdu, 1) != address || get_u16(*pdu, 3) != (on ? 0xFF00 : 0x0000)) {
    return protocol_error("modbus: write echo does not match the request", reply);
  }
  return {};
}

Result<void> decode_write_multiple_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start,
                                             std::uint16_t count, const Bytes& reply) {
  auto pdu = reply_pdu(tid, unit, Function::WriteMultipleRegisters, reply);
  if (!pdu) return fail(std::move(pdu).error());
  if (pdu->size() != 5 || get_u16(*pdu, 1) != start || get_u16(*pdu, 3) != count) {
    return protocol_error("modbus: write echo does not match the request", reply);
  }
  return {};
}

std::string_view to_string(WordOrder order) noexcept {
  switch (order) {
    case WordOrder::ABCD:
      return "abcd";
    case WordOrder::CDAB:
      return "cdab";
    case WordOrder::BADC:
      return "badc";
    case WordOrder::DCBA:
      return "dcba";
  }
  return "?";
}

std::optional<WordOrder> word_order_from_string(std::string_view text) noexcept {
  std::string lower;
  for (char c : text) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (auto o : {WordOrder::ABCD, WordOrder::CDAB, WordOrder::BADC, WordOrder::DCBA}) {
    if (lower == to_string(o)) return o;
  }
  return std::nullopt;
}

std::array<std::uint16_t, 2> encode_float(float value, WordOrder order) noexcept {
  return words_of(std::bit_cast<std::uint32_t>(value), order);
}

float decode_float(std::uint16_t first, std::uint16_t second, WordOrder order) noexcept {
  return std::bit_cast<float>(bits_of(first, second, order));
}

std::array<std::uint16_t, 2> encode_int32(std::int32_t value, WordOrder order) noexcept {
  return words_of(std::bit_cast<std::uint32_t>(value), order);
}

std::int32_t decode_int32(std::uint16_t first, std::uint16_t second, WordOrder order) noexcept {
  return std::bit_cast<std::int32_t>(bits_of(first, second, order));
}

std::optional<std::uint8_t> request_function(const Bytes& tx) noexcept {
  if (tx.size() < kMbapSize + 1) return std::nullopt;
  return tx[kMbapSize];
}

Result<Request> decode_request(const Bytes& tx) {
  if (tx.size() < kMbapSize + 5) return protocol_error("modbus: short request", tx);
  if (get_u16(tx, 2) != 0 || get_u16(tx, 4) != tx.size() - 6) return protocol_error("modbus: bad MBAP header", tx);
  Request r;
  r.tid = get_u16(tx, 0);
  r.unit = tx[6];
  r.function = tx[kMbapSize];
  r.start = get_u16(tx, kMbapSize + 1);
  const std::uint16_t word = get_u16(tx, kMbapSize + 3);
  switch (static_cast<Function>(r.function)) {
    case Function::ReadCoils:
    case Function::ReadHoldingRegisters:
    case Function::ReadInputRegisters:
      if (tx.size() != kMbapSize + 5) return protocol_error("modbus: bad read request length", tx);
      r.count = word;
      return r;
    case Function::WriteSingleCoil:
      if (tx.size() != kMbapSize + 5) return protocol_error("modbus: bad write request length", tx);
      if (word != 0xFF00 && word != 0x0000) return protocol_error("modbus: coil value is not FF00 or 0000", tx);
      r.count = 1;
      r.coil = word == 0xFF00;
      return r;
    case Function::WriteMultipleRegisters: {
      r.count = word;
      if (tx.size() < kMbapSize + 6 || tx[kMbapSize + 5] != 2 * r.count ||
          tx.size() != kMbapSize + 6 + 2 * static_cast<std::size_t>(r.count)) {
        return protocol_error("modbus: bad write request length", tx);
      }
      for (std::size_t i = 0; i < r.count; ++i) r.values.push_back(get_u16(tx, kMbapSize + 6 + 2 * i));
      return r;
    }
  }
  return protocol_error("modbus: unsupported function", tx);
}

Bytes encode_coils(std::uint16_t tid, std::uint8_t unit, const std::vector<bool>& coils) {
  const std::size_t bytes = (coils.size() + 7) / 8;
  Bytes pdu{code(Function::ReadCoils), static_cast<std::uint8_t>(bytes)};
  pdu.resize(2 + bytes, 0);
  for (std::size_t i = 0; i < coils.size(); ++i) {
    if (coils[i]) pdu[2 + i / 8] = static_cast<std::uint8_t>(pdu[2 + i / 8] | (1U << (i % 8)));
  }
  return frame(tid, unit, pdu);
}

Bytes encode_registers(std::uint16_t tid, std::uint8_t unit, Function function,
                       const std::vector<std::uint16_t>& registers) {
  Bytes pdu{code(function), static_cast<std::uint8_t>(2 * registers.size())};
  for (auto r : registers) put_u16(pdu, r);
  return frame(tid, unit, pdu);
}

Bytes encode_write_single_coil(std::uint16_t tid, std::uint8_t unit, std::uint16_t address, bool on) {
  Bytes pdu{code(Function::WriteSingleCoil)};
  put_u16(pdu, address);
  put_u16(pdu, on ? 0xFF00 : 0x0000);
  return frame(tid, unit, pdu);
}

Bytes encode_write_multiple_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start, std::uint16_t count) {
  Bytes pdu{code(Function::WriteMultipleRegisters)};
  put_u16(pdu, start);
  put_u16(pdu, count);
  return frame(tid, unit, pdu);
}

Bytes encode_exception(std::uint16_t tid, std::uint8_t unit, std::uint8_t function, std::uint8_t c) {
  return frame(tid, unit, Bytes{static_cast<std::uint8_t>(function | kExceptionFlag), c});
}

}  // namespace pychron::codec::modbus
