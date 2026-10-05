#pragma once

// Modbus application protocol over TCP (MBAP framing). See CONVENTIONS.md.
// Hand-rolled rather than libmodbus, which owns its socket (priorities doc,
// library decisions).
//
//   host -> MBAP{tid, 0, len, unit} <fn> <pdu...>
//   dev  -> MBAP{tid, 0, len, unit} <fn> <reply...>
//   dev  -> MBAP{tid, 0, 3, unit} <fn|0x80> <exception code>
//
// Functions: read coils (01), read holding registers (03), read input
// registers (04), write single coil (05), write multiple registers (16).
// Addresses are the 0-based protocol addresses; a driver that reads
// 1-based numbers from config subtracts its own offset.
//
// The PDU is built and checked apart from the MBAP header, so an RTU
// framing (address + PDU + CRC-16) can be added without changing drivers.
//
// Every decoder checks the transaction id, protocol id, length, unit id and
// function of the reply against the request it answers: on a connection
// several drivers share, a late reply to someone else's request must never
// be taken for this one's.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::modbus {

enum class Function : std::uint8_t {
  ReadCoils = 0x01,
  ReadHoldingRegisters = 0x03,
  ReadInputRegisters = 0x04,
  WriteSingleCoil = 0x05,
  WriteMultipleRegisters = 0x10,
};

inline constexpr std::uint8_t kExceptionFlag = 0x80;
inline constexpr std::uint16_t kMaxReadCoils = 2000;
inline constexpr std::uint16_t kMaxReadRegisters = 125;
inline constexpr std::uint16_t kMaxWriteRegisters = 123;
inline constexpr std::uint16_t kDefaultPort = 502;

// "illegal data address" for 2; "unlisted exception" for a code the spec
// does not name.
std::string exception_name(std::uint8_t code);

// --- host side ----------------------------------------------------------------

// Config error for a count outside the function's limit or a range past
// address 0xFFFF.
Result<Command> read_coils(std::uint16_t tid, std::uint8_t unit, std::uint16_t start, std::uint16_t count);
Result<Command> read_holding_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start,
                                       std::uint16_t count);
Result<Command> read_input_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start, std::uint16_t count);
Command write_single_coil(std::uint16_t tid, std::uint8_t unit, std::uint16_t address, bool on);
Result<Command> write_multiple_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start,
                                         const std::vector<std::uint16_t>& values);

// Replies. An exception reply, a mismatched id or function, and a malformed
// frame are Protocol errors; an exception says its code and name
// ("exception 2 (illegal data address)").
Result<std::vector<bool>> decode_coils(std::uint16_t tid, std::uint8_t unit, std::uint16_t count, const Bytes& reply);
// `function`: ReadHoldingRegisters or ReadInputRegisters, as requested.
Result<std::vector<std::uint16_t>> decode_registers(std::uint16_t tid, std::uint8_t unit, Function function,
                                                    std::uint16_t count, const Bytes& reply);
// The device echoes the request; any difference is a Protocol error.
Result<void> decode_write_single_coil(std::uint16_t tid, std::uint8_t unit, std::uint16_t address, bool on,
                                      const Bytes& reply);
Result<void> decode_write_multiple_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start,
                                             std::uint16_t count, const Bytes& reply);

// --- 32-bit values in two registers ------------------------------------------
// The value's big-endian bytes are A B C D (A most significant); the order
// names what the lower-addressed register holds first. ABCD: high word
// first. CDAB: low word first (legacy pychron's default: byte order big,
// word order little). BADC and DCBA swap the bytes in each word.

enum class WordOrder { ABCD, CDAB, BADC, DCBA };

std::string_view to_string(WordOrder order) noexcept;
// "abcd", "cdab", "badc", "dcba", any case; nullopt otherwise.
std::optional<WordOrder> word_order_from_string(std::string_view text) noexcept;

std::array<std::uint16_t, 2> encode_float(float value, WordOrder order) noexcept;
float decode_float(std::uint16_t first, std::uint16_t second, WordOrder order) noexcept;
std::array<std::uint16_t, 2> encode_int32(std::int32_t value, WordOrder order) noexcept;
std::int32_t decode_int32(std::uint16_t first, std::uint16_t second, WordOrder order) noexcept;

// --- device side, for simulation hooks ---------------------------------------

struct Request {
  std::uint16_t tid = 0;
  std::uint8_t unit = 0;
  std::uint8_t function = 0;
  std::uint16_t start = 0;  // the address for WriteSingleCoil
  std::uint16_t count = 0;  // 1 for WriteSingleCoil
  bool coil = false;        // WriteSingleCoil
  std::vector<std::uint16_t> values;  // WriteMultipleRegisters

  friend bool operator==(const Request&, const Request&) = default;
};

// One complete request of the five functions. Protocol error if malformed
// or another function (a sim answers that with an illegal-function exception).
Result<Request> decode_request(const Bytes& tx);
// The function code of a request frame, if it has one; for answering a
// request decode_request() refuses.
std::optional<std::uint8_t> request_function(const Bytes& tx) noexcept;

Bytes encode_coils(std::uint16_t tid, std::uint8_t unit, const std::vector<bool>& coils);
// `function`: ReadHoldingRegisters or ReadInputRegisters.
Bytes encode_registers(std::uint16_t tid, std::uint8_t unit, Function function,
                       const std::vector<std::uint16_t>& registers);
Bytes encode_write_single_coil(std::uint16_t tid, std::uint8_t unit, std::uint16_t address, bool on);
Bytes encode_write_multiple_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start, std::uint16_t count);
Bytes encode_exception(std::uint16_t tid, std::uint8_t unit, std::uint8_t function, std::uint8_t code);

}  // namespace pychron::codec::modbus
