#pragma once

// Agilent / Keysight 34970A-family data acquisition unit (34970A, 34972A,
// DAQ970A), SCPI over RS-232 or LAN. See CONVENTIONS.md. Wire form follows
// legacy pychron (hardware/agilent/agilent_gp_actuator.py, agilent_mixin.py):
//
//   host -> "*IDN?\n"                     identity
//   unit -> "Agilent Technologies,34970A,MY12345678,13-2-2\n"
//   host -> "ROUT:OPEN (@101)\n"          open relay 01 in slot 1 (no reply)
//   host -> "ROUT:CLOSE (@101)\n"         close it (no reply)
//   host -> "ROUT:OPEN? (@101)\n"         is it open?
//   unit -> "1\n"                         1 yes, 0 no
//   host -> "SYST:ERR?\n"                 oldest queued error
//   unit -> "+0,\"No error\"\n"           queue empty
//   unit -> "-113,\"Undefined header\"\n"
//
// Commands end in LF (legacy forces it); replies are framed at LF and a CR
// before it is ignored. A channel is three digits: slot 1..3, then channel
// 01..99 (a 34903A switch card has 01..20).
//
// Never sent: *TST? (legacy's identify), a full self-test that can cycle
// relays (owner decision 2026-10-05).

#include <optional>
#include <string>
#include <string_view>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::agilent {

inline constexpr std::string_view kTerminator = "\n";
const ReadSpec& reply_spec() noexcept;

// --- channels -------------------------------------------------------------------

// The channel as written in a valve address ("101", " 312 "), trimmed;
// Config error unless it is slot 1..3 followed by two digits other than 00.
Result<std::string> channel(std::string_view address);

// --- commands -------------------------------------------------------------------

Command identify();     // *IDN?
Command clear_status(); // *CLS (no reply)
Command next_error();   // SYST:ERR?
// ROUT:OPEN (@ch) / ROUT:CLOSE (@ch): no reply. Config error for a bad channel.
Result<Command> route_open(std::string_view address);
Result<Command> route_close(std::string_view address);
// ROUT:OPEN? (@ch) / ROUT:CLOSE? (@ch).
Result<Command> query_open(std::string_view address);
Result<Command> query_close(std::string_view address);

// --- replies --------------------------------------------------------------------

struct Identity {
  std::string manufacturer;
  std::string model;
  std::string serial;
  std::string firmware;
  friend bool operator==(const Identity&, const Identity&) = default;
};
// Four comma-separated fields; Protocol error otherwise.
Result<Identity> decode_identity(const Bytes& reply);
// True when the manufacturer is Agilent, Keysight or Hewlett-Packard and the
// model is one of the 34970A family this codec speaks to.
bool is_switch_unit(const Identity& identity) noexcept;

// A route query's answer for one channel: "1" true, "0" false; anything
// else, including a list of several, is a Protocol error.
Result<bool> decode_route_state(const Bytes& reply);

// One SYST:ERR? answer: nullopt for +0 ("No error"), else the instrument's
// code and message. A reply not in `<code>,"<message>"` form is a Protocol
// error.
struct InstrumentError {
  int code = 0;
  std::string message;
  friend bool operator==(const InstrumentError&, const InstrumentError&) = default;
};
Result<std::optional<InstrumentError>> decode_error(const Bytes& reply);

// --- unit side, for simulators --------------------------------------------------

// A host command as the unit reads it: header and argument, e.g.
// "ROUT:OPEN?" and "(@101)". Header compared case-insensitively by callers.
struct Request {
  std::string header;
  std::string argument;  // trimmed; empty if none
};
Result<Request> decode_request(const Bytes& tx);
// The single channel of "(@101)"; nullopt for anything else.
std::optional<std::string> single_channel(std::string_view argument);

Bytes encode_line(std::string_view text);  // text + "\n"
Bytes encode_error(int code, std::string_view message);  // "-113,\"Undefined header\"\n"; code 0 is "+0,\"No error\""

}  // namespace pychron::codec::agilent
