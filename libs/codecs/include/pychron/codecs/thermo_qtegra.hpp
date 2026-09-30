#pragma once

// Thermo Qtegra RemoteControlServer ASCII line protocol. See CONVENTIONS.md.
//
//   host -> "SetMagnetDAC 4.5\n"     command, args space/comma separated
//   srv  -> "OK\n"                   (decode_ok)
//   srv  -> "ERROR: <message>\n"     any rejected command (Protocol error)
//   host -> "GetMagnetDAC\n"
//   srv  -> "4.5\n"                  (decode_number)
//   host -> "GetData\n"
//   srv  -> "H2,0.0012,H1,0.0034\n"  tagged pairs     (decode_pairs)
//
// Booleans travel as "True"/"False" (BlankBeam, GetMagnetMoving); detector
// protection uses "On"/"Off". Numbers are plain decimals; values are returned
// exactly as reported. Hardware parameter names are the Qtegra UI names
// ("Y-Symmetry Set"); the canonical map is keyed by the snake_case canonical
// SourceParam name (devices `to_string(SourceParam)`), because codecs may not
// depend on devices.
//
// Synthetic: this grammar is reconstructed from the legacy pychron driver
// and has not been checked against a bench capture.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::qtegra {

inline constexpr std::string_view kTerminator = "\n";

// Thermo integration times are 0.065536 s * 2^n, n in 0..10.
inline constexpr double kBaseIntegration = 0.065536;
inline constexpr int kMaxIntegrationExponent = 10;

// Nearest legal integration time (nearest in log scale, so 0.1 -> 0.131072
// is decided by ratio). Non-positive or non-finite input snaps to the minimum.
double snap_integration_time(double seconds) noexcept;

// One canonical <-> hardware name pair.
struct ParamName {
  std::string_view canonical;  // "y_symmetry"
  std::string_view hardware;   // "Y-Symmetry Set"
};
std::span<const ParamName> param_names() noexcept;
std::optional<std::string_view> hardware_name(std::string_view canonical) noexcept;
std::optional<std::string_view> canonical_name(std::string_view hardware) noexcept;

using Pairs = std::vector<std::pair<std::string, double>>;

// --- host side ---------------------------------------------------------------
// Names (detectors, parameters) must be non-empty and free of ',' and control
// characters; otherwise Config. Numeric arguments must be finite.

Result<Command> set_magnet_dac(double dac);
Result<Command> get_magnet_dac();
Result<Command> get_magnet_moving();
Result<Command> blank_beam(bool blank);
Result<Command> protect_detector(std::string_view detector, bool protect);
Result<Command> set_deflection(std::string_view detector, double volts);
Result<Command> get_deflection(std::string_view detector);
Result<Command> get_deflections();
Result<Command> set_gain(std::string_view detector, double gain);
Result<Command> get_gain(std::string_view detector);
Result<Command> set_ion_counter_voltage(double volts);
// Seconds; snapped to a legal value before encoding.
Result<Command> set_integration_time(double seconds);
Result<Command> get_integration_time();
Result<Command> get_data();
Result<Command> set_hv(double kv);
Result<Command> get_high_voltage();
Result<Command> set_parameter(std::string_view hardware_name, double value);
Result<Command> get_parameter(std::string_view hardware_name);
Result<Command> get_parameters();
Result<Command> reset();

// --- decoders ------------------------------------------------------------------

Result<void> decode_ok(const Bytes& reply);
Result<double> decode_number(const Bytes& reply);
Result<bool> decode_bool(const Bytes& reply);
// "tag,value,tag,value,...\n"; an empty body is an empty list.
Result<Pairs> decode_pairs(const Bytes& reply);

}  // namespace pychron::codec::qtegra
