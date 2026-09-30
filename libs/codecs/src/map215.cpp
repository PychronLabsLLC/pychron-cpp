#include "pychron/codecs/map215.hpp"

#include <cmath>
#include <string>

namespace pychron::codec::map215 {

namespace {

// Non-negative decimal integer made only of digits, or nullopt.
std::optional<std::int64_t> parse_unsigned(std::string_view s) {
  if (s.empty() || s.size() > 12) return std::nullopt;
  std::int64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + (c - '0');
  }
  return v;
}

}  // namespace

Result<Command> select_range(int range) {
  if (range < kMinRange || range > kMaxRange) {
    return fail(ErrorKind::Config, "map215: range " + std::to_string(range) + " is outside 0..9");
  }
  return Command::write_only(to_bytes("B" + std::to_string(range) + kTerminator));
}

Result<Command> write_code(std::int64_t code) {
  if (code < 0 || code > kMaxCode) {
    return fail(ErrorKind::Config, "map215: code " + std::to_string(code) + " is outside 0..65535");
  }
  return Command::write_only(to_bytes("W" + std::to_string(code) + kTerminator));
}

Result<std::int64_t> to_code(double volts, double full_scale) {
  if (!(full_scale > 0.0) || !std::isfinite(full_scale)) {
    return fail(ErrorKind::Config, "map215: full scale must be positive");
  }
  if (!std::isfinite(volts) || volts < 0.0 || volts > full_scale) {
    return fail(ErrorKind::Config, "map215: " + std::to_string(volts) + " V is outside 0.." +
                                       std::to_string(full_scale) + " V");
  }
  return static_cast<std::int64_t>(std::llround(volts / full_scale * static_cast<double>(kMaxCode)));
}

double to_volts(std::int64_t code, double full_scale) noexcept {
  return static_cast<double>(code) * full_scale / static_cast<double>(kMaxCode);
}

Result<Request> decode_request(const Bytes& tx) {
  std::string text = to_string(tx);
  if (text.size() < 3 || text.back() != kTerminator) return protocol_error("map215: bad frame", tx);
  auto value = parse_unsigned(std::string_view(text).substr(1, text.size() - 2));
  if (!value) return protocol_error("map215: bad value", tx);
  switch (text.front()) {
    case 'B':
      if (*value > kMaxRange) return protocol_error("map215: range out of range", tx);
      return Request{Request::Kind::SelectRange, *value};
    case 'W':
      if (*value > kMaxCode) return protocol_error("map215: code out of range", tx);
      return Request{Request::Kind::Write, *value};
    default:
      return protocol_error("map215: unknown command", tx);
  }
}

}  // namespace pychron::codec::map215
