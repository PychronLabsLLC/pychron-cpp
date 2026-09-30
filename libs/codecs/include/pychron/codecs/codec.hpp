#pragma once

// Shared vocabulary for vendor codecs. See libs/codecs/CONVENTIONS.md.
//
// A codec is pure functions over bytes: no I/O, threads, time or config. It
// encodes a command to a `Command` (bytes plus how the reply is framed) and
// decodes reply bytes to a value or a Protocol error.

#include <cmath>
#include <cstddef>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "pychron/core/error.hpp"
#include "pychron/transport/bytes.hpp"
#include "pychron/transport/read_spec.hpp"

namespace pychron::codec {

// What to put on the wire and how the transport knows the reply is complete.
// `reply` is empty for commands the device does not answer.
struct Command {
  Bytes tx;
  std::optional<ReadSpec> reply;

  static Command ascii(std::string_view text, ReadSpec reply) {
    return Command{to_bytes(text), std::move(reply)};
  }
  static Command write_only(Bytes tx) { return Command{std::move(tx), std::nullopt}; }

  bool expects_reply() const noexcept { return reply.has_value(); }

  friend bool operator==(const Command&, const Command&) = default;
};

// Protocol error for a reply the codec cannot accept. The offending bytes are
// appended escaped. Device attribution is left to the driver.
inline Unexpected<Error> protocol_error(std::string what) {
  return fail(ErrorKind::Protocol, std::move(what));
}

inline Unexpected<Error> protocol_error(std::string what, const Bytes& reply) {
  return fail(ErrorKind::Protocol, std::move(what) + ": reply \"" + escape(reply) + "\"");
}

// Reply text without its trailing `terminator`; Protocol error if absent.
inline Result<std::string> strip_terminator(const Bytes& reply, std::string_view terminator) {
  std::string text = to_string(reply);
  if (text.size() < terminator.size() ||
      std::string_view(text).substr(text.size() - terminator.size()) != terminator) {
    return protocol_error("missing terminator", reply);
  }
  text.resize(text.size() - terminator.size());
  return text;
}

// True for [+-]digits[.digits][(E|e)[+-]digits] with at least one mantissa
// digit. Checked by hand so parsing never depends on the process locale.
inline bool is_decimal_number(std::string_view s) noexcept {
  auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
  std::size_t i = 0;
  auto digits = [&] {
    std::size_t start = i;
    while (i < s.size() && is_digit(s[i])) ++i;
    return i - start;
  };
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  std::size_t mantissa = digits();
  if (i < s.size() && s[i] == '.') {
    ++i;
    mantissa += digits();
  }
  if (mantissa == 0) return false;
  if (i < s.size() && (s[i] == 'E' || s[i] == 'e')) {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    if (digits() == 0) return false;
  }
  return i == s.size();
}

// A finite decimal number (is_decimal_number grammar), or nullopt.
inline std::optional<double> parse_decimal(std::string_view s) {
  if (!is_decimal_number(s)) return std::nullopt;
  std::istringstream in{std::string(s)};
  in.imbue(std::locale::classic());
  double v = 0.0;
  in >> v;
  if (in.fail() || !std::isfinite(v)) return std::nullopt;
  return v;
}

}  // namespace pychron::codec
