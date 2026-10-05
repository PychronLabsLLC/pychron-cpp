#pragma once

// Varian / Agilent XGS-600 gauge controller, ASCII over RS-232/485. See
// CONVENTIONS.md. Wire form follows legacy pychron
// (hardware/gauges/varian/varian_gauge_controller.py):
//
//   host -> "#0002UIG1\r"     read the sensor labelled IG1 (address 00)
//   xgs  -> ">1.234E-07\r"    its pressure in the controller's units
//   xgs  -> ">OFF\r"          the sensor is off
//   xgs  -> "?FF\r"           the controller rejected the command
//
// Not yet checked against a capture; what the controller says for a sensor
// that is over or under range is not known, so any answer that is not a
// number is a Protocol error quoting it ("gauge off" for OFF). Never a
// number in place of one.

#include <string>
#include <string_view>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::varian_xgs600 {

inline constexpr std::string_view kTerminator = "\r";
const ReadSpec& reply_spec() noexcept;

// `address`: two hex digits ("00" on RS-232). `label`: the sensor's user
// label, 1..8 letters and digits. Config error otherwise.
Result<Command> read_pressure(std::string_view address, std::string_view label);
Result<void> validate_address(std::string_view address);
Result<void> validate_label(std::string_view label);

Result<double> decode_pressure(const Bytes& reply);

// --- controller side, for simulators ----------------------------------------------

// The label of a "#aa02U<label>" request at `address`, or nullopt.
std::optional<std::string> requested_label(std::string_view address, const Bytes& tx);
Bytes encode_pressure(double value);  // ">1.234E-07\r"
Bytes encode_off();                   // ">OFF\r"
Bytes encode_rejected();              // "?FF\r"

}  // namespace pychron::codec::varian_xgs600
