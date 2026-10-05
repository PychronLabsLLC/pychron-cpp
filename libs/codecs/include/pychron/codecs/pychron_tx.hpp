#pragma once

// Legacy Pychron's valve service (pychron/tx/protocols/base_valve.py), for
// one Pychron switching valves through another (felix serves jan). See
// CONVENTIONS.md.
//
//   host -> "Open A\r"               open valve A (the remote's valve name)
//   peer -> "OK"                     done; "ok": it already was
//   host -> "Close A\r"
//   host -> "GetValveState A\r"
//   peer -> "OK"                     open (the server answers Python True as OK)
//   peer -> "False"                  closed
//   peer -> "ERROR 005 : A is not a registered valve name"
//
// The server writes its reply with no terminator and then closes the
// connection: one command per connection, the reply framed by the close
// (ReadSpec::until_close()).
//
// Error codes (pychron/tx/errors.py) map to kinds: 012 system lock and 014
// software lock are Interlock; 003 invalid command, 004 invalid arguments
// and 005 unknown valve are Config; anything else, 015 failed to actuate
// among them, is Protocol. The message keeps the server's text.

#include <string_view>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::pychron_tx {

inline constexpr std::string_view kTerminator = "\r";
const ReadSpec& reply_spec() noexcept;

// Config error for an empty name, a comma (the server splits arguments at
// commas) or a control character.
Result<Command> open_valve(std::string_view name);
Result<Command> close_valve(std::string_view name);
Result<Command> get_valve_state(std::string_view name);

// "OK" or "ok"; an ERROR reply as above; anything else is Protocol.
Result<void> decode_actuation(const Bytes& reply);
// "OK" or "True" open, "False" closed; an ERROR reply as above; anything
// else, "No Response" among them, is Protocol.
Result<bool> decode_state(const Bytes& reply);

// --- server side, for simulators ----------------------------------------------

struct Request {
  std::string verb;
  std::string name;
};
// "Verb name", trimmed; Protocol error for an empty line.
Result<Request> decode_request(const Bytes& tx);
Bytes encode_ok(bool changed);  // "OK" / "ok"
Bytes encode_state(bool open);  // "OK" / "False"
Bytes encode_error(std::string_view code, std::string_view message);  // "ERROR 005 : message"

}  // namespace pychron::codec::pychron_tx
