#pragma once

#include <cstddef>
#include <optional>

#include "pychron/transport/bytes.hpp"

namespace pychron {

// How a transport knows a reply is complete. Framing only, never meaning.
struct ReadSpec {
  enum class Kind {
    Terminator,   // read up to and including `terminator`
    FixedLength,  // read exactly `length` bytes
    ModbusRtu,    // addr, fn, payload, CRC16; length derived from fn / byte count
    ModbusTcp,    // MBAP header; length derived from its length field
  };

  Kind kind = Kind::Terminator;
  Bytes terminator;
  std::size_t length = 0;

  static ReadSpec until(Bytes terminator);
  static ReadSpec until(std::string_view terminator);
  static ReadSpec fixed(std::size_t length);
  static ReadSpec modbus_rtu();
  static ReadSpec modbus_tcp();

  friend bool operator==(const ReadSpec&, const ReadSpec&) = default;
};

// If `buffer` begins with a complete frame per `spec`, its length in bytes;
// otherwise nullopt (need more bytes). A Terminator spec with an empty
// terminator, or FixedLength 0, is complete immediately with 0 bytes.
std::optional<std::size_t> frame_length(const ReadSpec& spec, const Bytes& buffer);

}  // namespace pychron
