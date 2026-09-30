#pragma once

// Shared vocabulary for vendor codecs. See libs/codecs/CONVENTIONS.md.
//
// A codec is pure functions over bytes: no I/O, threads, time or config. It
// encodes a command to a `Command` (bytes plus how the reply is framed) and
// decodes reply bytes to a value or a Protocol error.

#include <optional>
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

}  // namespace pychron::codec
