#include "pychron/transport/bytes.hpp"

namespace pychron {

namespace {

constexpr char kHex[] = "0123456789abcdef";

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

Bytes to_bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }

std::string to_string(const Bytes& b) { return std::string(b.begin(), b.end()); }

std::string to_hex(const Bytes& b) {
  std::string out;
  out.reserve(b.size() * 2);
  for (auto byte : b) {
    out += kHex[byte >> 4];
    out += kHex[byte & 0x0f];
  }
  return out;
}

std::optional<Bytes> from_hex(std::string_view hex) {
  if (hex.size() % 2 != 0) return std::nullopt;
  Bytes out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hex_value(hex[i]);
    const int lo = hex_value(hex[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<std::uint8_t>(hi << 4 | lo));
  }
  return out;
}

std::string escape(const Bytes& b) {
  std::string out;
  for (auto byte : b) {
    switch (byte) {
      case '\r': out += "\\r"; break;
      case '\n': out += "\\n"; break;
      case '\\': out += "\\\\"; break;
      default:
        if (byte >= 0x20 && byte < 0x7f) {
          out += static_cast<char>(byte);
        } else {
          out += "\\x";
          out += kHex[byte >> 4];
          out += kHex[byte & 0x0f];
        }
    }
  }
  return out;
}

}  // namespace pychron
