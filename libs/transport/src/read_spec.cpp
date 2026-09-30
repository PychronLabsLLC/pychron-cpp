#include "pychron/transport/read_spec.hpp"

#include <algorithm>

namespace pychron {

ReadSpec ReadSpec::until(Bytes terminator) {
  ReadSpec s;
  s.kind = Kind::Terminator;
  s.terminator = std::move(terminator);
  return s;
}

ReadSpec ReadSpec::until(std::string_view terminator) { return until(to_bytes(terminator)); }

ReadSpec ReadSpec::until_any(Bytes bytes) {
  ReadSpec s;
  s.kind = Kind::AnyOf;
  s.terminator = std::move(bytes);
  return s;
}

ReadSpec ReadSpec::until_any(std::string_view bytes) { return until_any(to_bytes(bytes)); }

ReadSpec ReadSpec::fixed(std::size_t length) {
  ReadSpec s;
  s.kind = Kind::FixedLength;
  s.length = length;
  return s;
}

ReadSpec ReadSpec::modbus_rtu() {
  ReadSpec s;
  s.kind = Kind::ModbusRtu;
  return s;
}

ReadSpec ReadSpec::modbus_tcp() {
  ReadSpec s;
  s.kind = Kind::ModbusTcp;
  return s;
}

namespace {

std::optional<std::size_t> complete_if(std::size_t needed, std::size_t have) {
  if (have >= needed) return needed;
  return std::nullopt;
}

// RTU reply length from the function code: exceptions are addr+fn+code+crc,
// reads carry a byte count, writes echo address/quantity.
std::optional<std::size_t> modbus_rtu_length(const Bytes& b) {
  if (b.size() < 2) return std::nullopt;
  const auto fn = b[1];
  if (fn & 0x80) return complete_if(5, b.size());
  switch (fn) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x17:
      if (b.size() < 3) return std::nullopt;
      return complete_if(3u + b[2] + 2u, b.size());
    case 0x05: case 0x06: case 0x0F: case 0x10:
      return complete_if(8, b.size());
    default:
      // Unknown function: fall back to the byte-count form.
      if (b.size() < 3) return std::nullopt;
      return complete_if(3u + b[2] + 2u, b.size());
  }
}

}  // namespace

std::optional<std::size_t> frame_length(const ReadSpec& spec, const Bytes& buffer) {
  switch (spec.kind) {
    case ReadSpec::Kind::Terminator: {
      if (spec.terminator.empty()) return 0;
      auto it = std::search(buffer.begin(), buffer.end(), spec.terminator.begin(), spec.terminator.end());
      if (it == buffer.end()) return std::nullopt;
      return static_cast<std::size_t>(it - buffer.begin()) + spec.terminator.size();
    }
    case ReadSpec::Kind::AnyOf: {
      if (spec.terminator.empty()) return 0;
      auto in_set = [&](std::uint8_t b) {
        return std::find(spec.terminator.begin(), spec.terminator.end(), b) != spec.terminator.end();
      };
      auto body = std::find_if_not(buffer.begin(), buffer.end(), in_set);
      if (body == buffer.end()) return std::nullopt;
      auto it = std::find_if(body, buffer.end(), in_set);
      if (it == buffer.end()) return std::nullopt;
      return static_cast<std::size_t>(it - buffer.begin()) + 1;
    }
    case ReadSpec::Kind::FixedLength:
      return complete_if(spec.length, buffer.size());
    case ReadSpec::Kind::ModbusRtu:
      return modbus_rtu_length(buffer);
    case ReadSpec::Kind::ModbusTcp: {
      if (buffer.size() < 6) return std::nullopt;
      const std::size_t len = static_cast<std::size_t>(buffer[4]) << 8 | buffer[5];
      return complete_if(6 + len, buffer.size());
    }
  }
  return std::nullopt;
}

}  // namespace pychron
