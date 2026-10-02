#pragma once

// Thermo Qtegra RemoteControlServer ASCII line protocol. See CONVENTIONS.md.
// Wire format follows the production pychron Python driver
// (pychron/spectrometer/thermo/*, hardware/core/communicators/*):
//
//   host -> "SetMagnetDAC 4.5\r"     command, args space/comma separated,
//                                    write terminator "\r" by default
//   srv  -> "OK\r\n"                 (decode_ok; "ok" in any case)
//   srv  -> "ERROR: <message>"       rejected command (Protocol error)
//   host -> "GetMagnetDAC\r"
//   srv  -> "4.5\r\n"                (decode_number)
//   host -> "GetData\r"
//   srv  -> "H2,0.0012,H1,0.0034"    tagged pairs      (decode_data)
//   srv  -> "0.0012,0.0034,..."      untagged, fixed detector order
//   host -> "GetDeflections H1,AX\r"
//   srv  -> "12.5,-3"                bare CSV in request order
//                                    (decode_named_values)
//
// Python does not frame replies by a terminator; it strips the reply. Replies
// here are framed by CR or LF (`reply_spec()`, ReadSpec::until_any) and every
// decoder trims surrounding whitespace, so "\r", "\n" and "\r\n" all work.
//
// Booleans are sent as "True"/"False" (BlankBeam); boolean replies accept
// pychron's to_bool vocabulary. Detector protection uses "On"/"Off". Numbers
// are sent in Python str(float) form (shortest round trip). Hardware parameter
// names are the Qtegra names ("Y-Symmetry Set"); the map is keyed by the
// snake_case canonical SourceParam name (devices `to_string(SourceParam)`),
// because codecs may not depend on devices.
//
// Not yet checked against a bench capture.

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::qtegra {

// Write terminator appended to every command. pychron's Communicator default
// is "\r" (write_terminator = chr(13)).
enum class Terminator { CR, LF, CRLF };
inline constexpr Terminator kDefaultTerminator = Terminator::CR;
std::string_view terminator_text(Terminator t) noexcept;

// Reply framing: complete at the first CR or LF (leading CR/LF absorbed).
const ReadSpec& reply_spec() noexcept;

// Legal Qtegra integration times (pychron QTEGRA_INTEGRATION_TIMES),
// 0.065536 s * 2^n, n in 0..10.
inline constexpr std::array<double, 11> kIntegrationTimes{
    0.065536, 0.131072, 0.262144, 0.524288, 1.048576, 2.097152,
    4.194304, 8.388608, 16.777216, 33.554432, 67.108864};

// Nearest legal integration time by linear distance (pychron
// normalize_integration_time; ties go to the shorter time). Non-finite input
// snaps to the minimum, as numpy argmin does.
double snap_integration_time(double seconds) noexcept;

// Python str(float) form: shortest round-trip digits, fixed notation for
// decimal exponents in [-4, 16), scientific ("1e-05") otherwise. Integral
// values print without ".0" ("120").
std::string format_number(double v);

// Canonical <-> hardware name entry. A canonical name may appear more than
// once; the first entry is the preferred (pychron Python) name and later ones
// are aliases accepted by canonical_name().
struct ParamName {
  std::string_view canonical;  // "trap_voltage"
  std::string_view hardware;   // "Trap Voltage Set"   (SetParameter/GetParameter)
  std::string_view readback;   // "Trap Voltage Readback", empty if none known
};
std::span<const ParamName> param_names() noexcept;
std::optional<std::string_view> hardware_name(std::string_view canonical) noexcept;
std::optional<std::string_view> readback_name(std::string_view canonical) noexcept;
// Matches set names, readback names and aliases.
std::optional<std::string_view> canonical_name(std::string_view hardware) noexcept;

using Pairs = std::vector<std::pair<std::string, double>>;

// Default detector order for untagged GetData (pychron read_intensities).
inline constexpr std::array<std::string_view, 6> kDefaultDetectorOrder{"H2", "H1", "AX", "L1", "L2", "CDD"};

// --- host side ---------------------------------------------------------------
// Names (detectors, parameters, configurations) must be non-empty and free of
// ',' and control characters; otherwise Config. Numeric arguments must be
// finite. Every encoder appends `term`.

// The name rule above, for checking configured names before first use.
Result<void> validate_name(std::string_view name);

Result<Command> set_magnet_dac(double dac,Terminator term = kDefaultTerminator);
Result<Command> get_magnet_dac(Terminator term = kDefaultTerminator);
Result<Command> get_magnet_moving(Terminator term = kDefaultTerminator);
Result<Command> blank_beam(bool blank, Terminator term = kDefaultTerminator);
// "ProtectDetector <det>,On|Off": the form ThermoMagnet.set_dac sends around a
// magnet move (magnet/base.py). The manager path (ThermoSpectrometerManager.
// protect_detector -> Spectrometer.set_parameter) instead sends
// "SetParameter ProtectDetector,<det>,On|Off"; see protect_detector_parameter.
Result<Command> protect_detector(std::string_view detector, bool protect, Terminator term = kDefaultTerminator);
Result<Command> protect_detector_parameter(std::string_view detector, bool protect,
                                           Terminator term = kDefaultTerminator);
Result<Command> set_deflection(std::string_view detector, double volts, Terminator term = kDefaultTerminator);
Result<Command> get_deflection(std::string_view detector, Terminator term = kDefaultTerminator);
// "GetDeflections H1,AX,..."; reply via decode_named_values(reply, detectors).
Result<Command> get_deflections(std::span<const std::string> detectors, Terminator term = kDefaultTerminator);
Result<Command> set_gain(std::string_view detector, double gain, Terminator term = kDefaultTerminator);
Result<Command> get_gain(std::string_view detector, Terminator term = kDefaultTerminator);
// Not seen in pychron Python; unverified.
Result<Command> set_ion_counter_voltage(double volts, Terminator term = kDefaultTerminator);
// Seconds; snapped to a legal value before encoding.
Result<Command> set_integration_time(double seconds, Terminator term = kDefaultTerminator);
Result<Command> get_integration_time(Terminator term = kDefaultTerminator);
Result<Command> get_data(Terminator term = kDefaultTerminator);
// Accelerating voltage in volts (e.g. Helix nominal 9900), sent as-is.
Result<Command> set_hv(double volts, Terminator term = kDefaultTerminator);
Result<Command> get_high_voltage(Terminator term = kDefaultTerminator);
Result<Command> set_parameter(std::string_view hardware_name, double value, Terminator term = kDefaultTerminator);
Result<Command> get_parameter(std::string_view hardware_name, Terminator term = kDefaultTerminator);
// "GetParameters name1,name2"; reply via decode_named_values(reply, names).
Result<Command> get_parameters(std::span<const std::string> hardware_names, Terminator term = kDefaultTerminator);
// Source lens commands (ThermoSource setters, source/base.py).
Result<Command> set_y_symmetry(double value, Terminator term = kDefaultTerminator);
Result<Command> set_z_symmetry(double value, Terminator term = kDefaultTerminator);
Result<Command> set_extraction_lens(double value, Terminator term = kDefaultTerminator);
// Helix z-symmetry readback (HelixSource.read_z_symmetry).
Result<Command> get_extraction_symmetry(Terminator term = kDefaultTerminator);
// "SetSubCupConfiguration <name>" (ThermoSpectrometer sub_cup_configuration).
Result<Command> set_sub_cup_configuration(std::string_view name, Terminator term = kDefaultTerminator);
// Not seen in pychron Python; unverified.
Result<Command> reset(Terminator term = kDefaultTerminator);

// --- decoders ------------------------------------------------------------------
// Replies are trimmed of surrounding whitespace (CR/LF/space/tab). A reply
// starting with "ERROR" is a Protocol error (GetData: containing "ERROR").

Result<void> decode_ok(const Bytes& reply);
// Acknowledgement of a command whose reply pychron ignores (SetMagnetDAC,
// BlankBeam, ProtectDetector, SetDeflection, SetGain): any reply, including
// an empty line, is success; only "ERROR..." is a Protocol error.
Result<void> decode_ack(const Bytes& reply);
Result<double> decode_number(const Bytes& reply);
// pychron to_bool, case-insensitive: true/t/yes/y/1/ok/open -> true,
// false/f/no/n/0/closed -> false; anything else is a Protocol error.
Result<bool> decode_bool(const Bytes& reply);
// Bare CSV of floats, one per requested name in the same order; returns the
// values paired with `names`. Count mismatch is a Protocol error.
Result<Pairs> decode_named_values(const Bytes& reply, std::span<const std::string> names);
// GetData, tagged: "tag,value,tag,value,..."; an empty body is an empty list.
Result<Pairs> decode_data(const Bytes& reply);
// GetData, untagged: bare CSV paired with `order` (count must match); an empty
// body is an empty list.
Result<Pairs> decode_data(const Bytes& reply, std::span<const std::string_view> order);

// --- server side (simulators) -------------------------------------------------

// One host command split the way the server reads it: the verb up to the
// first space, then comma-separated arguments ("SetParameter Trap Voltage
// Set,5" -> {"SetParameter", {"Trap Voltage Set", "5"}}). Trailing CR/LF is
// stripped; arguments are trimmed. An empty line is a Protocol error.
struct Request {
  std::string verb;
  std::vector<std::string> args;
};
Result<Request> decode_request(const Bytes& tx);

// Replies end with "\r\n".
Bytes encode_ok();                             // "OK"
Bytes encode_number(double v);                 // format_number(v)
Bytes encode_bool(bool v);                     // "True" / "False"
Bytes encode_error(std::string_view message);  // "ERROR: <message>"

}  // namespace pychron::codec::qtegra
