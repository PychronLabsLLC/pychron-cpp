#pragma once

// Serial high-voltage supply for a legacy ion source (Spellman-style ASCII
// front end). See CONVENTIONS.md.
//
//   host   -> "VSET 4500.0\r"   program the output      (set_voltage)
//   supply -> "OK\r"                                     (decode_ok)
//   host   -> "VSET?\r"         programmed setpoint      (read_setpoint)
//   host   -> "VOUT?\r"         measured output          (read_output)
//   supply -> "4499.8\r"                                 (decode_voltage)
//   supply -> "ERR <message>\r" rejected command         (Protocol error)
//
// Volts, as the supply reports them.

#include <string_view>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::hv_supply {

inline constexpr std::string_view kTerminator = "\r";

// --- host side --------------------------------------------------------------

// "VSET <v:.1f>\r"; Config error for negative or non-finite volts.
Result<Command> set_voltage(double volts);
Command read_setpoint();
Command read_output();

Result<void> decode_ok(const Bytes& reply);
Result<double> decode_voltage(const Bytes& reply);

// --- supply side, for simulation hooks ------------------------------------------

struct Request {
  enum class Kind { Set, ReadSetpoint, ReadOutput };
  Kind kind = Kind::ReadOutput;
  double volts = 0.0;  // Set only

  friend bool operator==(const Request&, const Request&) = default;
};

Result<Request> decode_request(const Bytes& tx);
Bytes encode_ok();
// "<v:.1f>\r".
Bytes encode_voltage(double volts);
Bytes encode_error(std::string_view message);

}  // namespace pychron::codec::hv_supply
