#pragma once

// Lake Shore 325/331/335/336 cryogenic temperature controllers, the
// GPIB-style command set over serial (legacy hardware/lakeshore, GPIBProtocol).
// See CONVENTIONS.md.
//
//   host -> "*IDN?\n"            unit -> "LSCI,MODEL335,1234567/1234567,1.0\r\n"
//   host -> "KRDG? A\n"          unit -> "+077.123\r\n"   input A in kelvin
//   host -> "CRDG? A\n"          the same in Celsius
//   host -> "SETP 1,77.000\n"    output 1's setpoint (no reply)
//   host -> "SETP? 1\n"          unit -> "+077.000\r\n"
//   host -> "RANGE 1,2\n"        output 1's heater range (no reply); 0 is off
//   host -> "RANGE? 1\n"         unit -> "2\r\n"
//
// Commands end in LF, as legacy forced; replies are framed at LF and a CR
// before it is ignored. Inputs are sent as upper-case letters (legacy sent
// lower case; the unit takes either). Numbers are written locale-free with
// three decimals. A reading that is not a number (an over-range or
// unconnected sensor) is a Protocol error quoting it.

#include <string>
#include <string_view>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::lakeshore {

inline constexpr std::string_view kTerminator = "\n";
const ReadSpec& reply_spec() noexcept;

enum class Units { Kelvin, Celsius };

// Config error for an input other than A..D, an output other than 1..4, a
// range other than 0..5, or a setpoint that is negative, not finite or
// above 2000.
Command identify();      // *IDN?
Command clear_status();  // *CLS (no reply)
Result<Command> read_input(char input, Units units = Units::Kelvin);  // KRDG? A / CRDG? A
Result<Command> set_setpoint(int output, double value);                // SETP 1,77.000
Result<Command> query_setpoint(int output);                            // SETP? 1
Result<Command> set_range(int output, int range);                      // RANGE 1,2
Result<Command> query_range(int output);                               // RANGE? 1

// "+077.000" style replies.
Result<double> decode_number(const Bytes& reply);
Result<int> decode_range(const Bytes& reply);

struct Identity {
  std::string manufacturer;  // "LSCI"
  std::string model;         // "MODEL335"
  std::string serial;
  std::string firmware;
  friend bool operator==(const Identity&, const Identity&) = default;
};
Result<Identity> decode_identity(const Bytes& reply);

// --- unit side, for simulators -------------------------------------------------

struct Request {
  std::string header;    // "KRDG?", "SETP", ... upper case
  std::string argument;  // trimmed, may be empty
};
Result<Request> decode_request(const Bytes& tx);
Bytes encode_number(double value);  // "+077.123\r\n"
Bytes encode_line(std::string_view text);

}  // namespace pychron::codec::lakeshore
