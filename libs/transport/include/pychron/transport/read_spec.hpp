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
    AnyOf,        // read up to and including the first byte found in `terminator`
                  // (a byte set, e.g. "\r\n" = CR or LF); leading set bytes are
                  // absorbed into the frame, so the LF left over from a "\r\n"
                  // reply is consumed by the next frame instead of ending it
    UntilClose,   // everything up to the peer closing the connection (a server
                  // that answers one command per connection, unterminated);
                  // never complete by content
  };

  Kind kind = Kind::Terminator;
  Bytes terminator;
  std::size_t length = 0;

  static ReadSpec until(Bytes terminator);
  static ReadSpec until(std::string_view terminator);
  // AnyOf: frame ends at the first byte from `bytes`.
  static ReadSpec until_any(Bytes bytes);
  static ReadSpec until_any(std::string_view bytes);
  static ReadSpec fixed(std::size_t length);
  static ReadSpec modbus_rtu();
  static ReadSpec modbus_tcp();
  static ReadSpec until_close();

  friend bool operator==(const ReadSpec&, const ReadSpec&) = default;
};

// If `buffer` begins with a complete frame per `spec`, its length in bytes;
// otherwise nullopt (need more bytes; always, for UntilClose). A Terminator or AnyOf spec with an
// empty terminator, or FixedLength 0, is complete immediately with 0 bytes.
// An AnyOf buffer holding only set bytes is incomplete.
std::optional<std::size_t> frame_length(const ReadSpec& spec, const Bytes& buffer);

}  // namespace pychron
