#pragma once

// Thermo Qtegra RemoteControlServer (RCS) ASCII line protocol. See
// CONVENTIONS.md. The contract is PROTOCOL.md and protocol.json in the qtegra
// repository; tests/traces/thermo/protocol.json is a copy, and a test fails
// when an encoder here sends what that file does not define.
//
// Standard path: one TCP connection, KeepAlive as its first command.
//
//   host -> "KeepAlive\r"            first on every connection, or RCS closes
//                                    the connection after one reply
//   srv  -> "OK\n"
//   host -> "Describe\r"
//   srv  -> "{\"protocol\":1,...}\n"  one line of JSON (decode_describe)
//   host -> "SetMagnetDAC 4.5\r"     command, args space/comma separated
//   srv  -> "OK\n"                   (decode_ok; "ok" in any case)
//   srv  -> "Error: <message>\n"     rejected command (Protocol error, any
//                                    case)
//   host -> "GetMagnetDAC\r"
//   srv  -> "4.5\n"                  (decode_number)
//   host -> "GetData\r"
//   srv  -> "H2,0.0012,H1,0.0034\n"  tagged pairs      (decode_data)
//   srv  -> "0.0012,0.0034,...\n"    untagged, fixed detector order
//   host -> "GetDeflections H1,AX\r"
//   srv  -> "12.5,-3\n"              bare CSV in request order
//                                    (decode_named_values)
//
// RCS ends a command at CR, LF or CRLF and a reply at LF only: a `lines` reply
// holds CRs (`reply_spec()`). Over UDP, and on a TCP connection without
// KeepAlive, replies have no line ending at all; neither is supported here.
//
// Booleans are sent as "true"/"false" (BlankBeam); boolean replies accept
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

// Reply framing on a KeepAlive connection: complete at the first LF. A CR is
// data (`lines` replies); a "\r\n" ending still frames, and decoders trim it.
const ReadSpec& reply_spec() noexcept;
// Request framing, for a simulated Qtegra cutting commands from a stream:
// complete at the first CR or LF (leading CR/LF absorbed), as RCS reads them,
// so every Terminator a driver may be set to ends a command.
const ReadSpec& request_spec() noexcept;

// The protocol version this driver speaks (Describe's `protocol`).
inline constexpr int kProtocolVersion = 1;
// Error::code of GetData's "Error: no data": no scan has arrived yet, which is
// not a failure of the request.
inline constexpr const char* kNoData = "qtegra_no_data";

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
//
// `verified` is true only when the production pychron Python driver sends
// `hardware` to an instrument (SetParameter / GetParameter / GetParameters).
// An unverified name is a guess: it stays in the map so a reply or a config
// naming it is recognised, but a driver must not send it. Every readback name
// in the map is one pychron Python reads.
struct ParamName {
  std::string_view canonical;  // "trap_voltage"
  std::string_view hardware;   // "Trap Voltage Set"   (SetParameter/GetParameter)
  std::string_view readback;   // "Trap Voltage Readback", empty if none known
  bool verified = false;       // pychron Python sends `hardware`
};
std::span<const ParamName> param_names() noexcept;
// True when `hardware` is a name pychron Python sends: the hardware name of a
// verified entry, or a readback name.
bool verified_name(std::string_view hardware) noexcept;
std::optional<std::string_view> hardware_name(std::string_view canonical) noexcept;
std::optional<std::string_view> readback_name(std::string_view canonical) noexcept;
// Matches set names, readback names and aliases.
std::optional<std::string_view> canonical_name(std::string_view hardware) noexcept;

using Pairs = std::vector<std::pair<std::string, double>>;

// Default detector order for untagged GetData (pychron read_intensities).
inline constexpr std::array<std::string_view, 6> kDefaultDetectorOrder{"H2", "H1", "AX", "L1", "L2", "CDD"};

// --- host side ---------------------------------------------------------------
// Names (detectors, parameters, configurations) must be non-empty and free of
// ',' and control characters; otherwise Config. Where protocol.json types a
// name `word` (ProtectDetector, ActivateCupConfiguration) it may hold no space
// either. Numeric arguments must be finite. Every encoder appends `term`.

// The name rule above, for checking configured names before first use.
Result<void> validate_name(std::string_view name);

// First command on a connection, and only there: RCS answers it anywhere else
// with an error.
Result<Command> keep_alive(Terminator term = kDefaultTerminator);
Result<Command> describe(Terminator term = kDefaultTerminator);
Result<Command> ping(Terminator term = kDefaultTerminator);
Result<Command> set_magnet_dac(double dac, Terminator term = kDefaultTerminator);
Result<Command> get_magnet_dac(Terminator term = kDefaultTerminator);
Result<Command> get_magnet_moving(Terminator term = kDefaultTerminator);
Result<Command> blank_beam(bool blank, Terminator term = kDefaultTerminator);
// "ProtectDetector <det>,On|Off". Not idempotent on RCS: On saves the deflection
// and sets 2000, so a second On saves 2000 and a later Off restores that. Never
// send it twice: see QtegraSpectrometer::protect.
Result<Command> protect_detector(std::string_view detector, bool protect, Terminator term = kDefaultTerminator);
Result<Command> set_deflection(std::string_view detector, double volts, Terminator term = kDefaultTerminator);
Result<Command> get_deflection(std::string_view detector, Terminator term = kDefaultTerminator);
// "GetDeflections H1,AX,..."; reply via decode_named_values(reply, detectors).
Result<Command> get_deflections(std::span<const std::string> detectors, Terminator term = kDefaultTerminator);
Result<Command> set_gain(std::string_view detector, double gain, Terminator term = kDefaultTerminator);
Result<Command> get_gain(std::string_view detector, Terminator term = kDefaultTerminator);
// RCS sets "CDD Supply Set".
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
// Source lens parameters. RCS has no command of their own (legacy pychron's
// SetYSymmetry, SetZSymmetry, SetExtractionLens, GetExtractionSymmetry): each
// is SetParameter or GetParameter of its hardware name.
Result<Command> set_y_symmetry(double value, Terminator term = kDefaultTerminator);
Result<Command> set_z_symmetry(double value, Terminator term = kDefaultTerminator);
Result<Command> set_extraction_lens(double value, Terminator term = kDefaultTerminator);
Result<Command> get_extraction_symmetry(Terminator term = kDefaultTerminator);
// "ActivateCupConfiguration <cup> <sub_cup>". RCS has no SetSubCupConfiguration
// and no Reset, so neither is encoded here.
Result<Command> activate_cup_configuration(std::string_view cup, std::string_view sub_cup,
                                           Terminator term = kDefaultTerminator);

// Valves through Qtegra (legacy ASCIIGPActuator / QtegraGPActuator):
// "Open <name>", "Close <name>" answered "OK"; "GetValveState <name>"
// answered "True" or "False". Names are Qtegra's ("Valve 1_9 Set") and may
// contain spaces.
Result<Command> open_valve(std::string_view name, Terminator term = kDefaultTerminator);
Result<Command> close_valve(std::string_view name, Terminator term = kDefaultTerminator);
Result<Command> get_valve_state(std::string_view name, Terminator term = kDefaultTerminator);

// --- decoders ------------------------------------------------------------------
// Replies are trimmed of surrounding whitespace (CR/LF/space/tab). A reply
// starting with "Error", in any case, is a Protocol error holding the message
// (GetData: containing it).

// What Describe says about the server.
struct Detector {
  std::string id;    // "CUP 1"
  std::string name;  // "H1", as GetData tags it
};
struct ServerInfo {
  int protocol = 0;
  std::string version;     // of the script
  std::string instrument;  // "HelixSFT"
  std::string transport;   // "tcp" or "udp"
  bool tag_data = false;   // GetData sends name,value pairs
  std::vector<std::string> features;
  std::vector<Detector> detectors;
};

// The one-line JSON. "Error: Invalid Command" means an RCS older than Describe,
// and is a Protocol error saying so; so is any other error, a reply that is not
// JSON, and a missing or mistyped `protocol`, `tag_data` or `detectors`.
Result<ServerInfo> decode_describe(const Bytes& reply);
// Protocol error unless this driver can use `server`: a protocol other than
// kProtocolVersion (newer: replies may differ in ways not handled here; older:
// a version before any this driver knows), or GetData without tags, which
// decode_data must have to attribute a value to a detector.
Result<void> check_server(const ServerInfo& server);

Result<void> decode_ok(const Bytes& reply);
// Acknowledgement of a command whose reply pychron ignores (SetMagnetDAC,
// BlankBeam, ProtectDetector, SetDeflection, SetGain): any reply, including
// an empty line, is success; only "Error..." is a Protocol error.
Result<void> decode_ack(const Bytes& reply);
Result<double> decode_number(const Bytes& reply);
// pychron to_bool, case-insensitive: true/t/yes/y/1/ok/open -> true,
// false/f/no/n/0/closed -> false; anything else is a Protocol error.
Result<bool> decode_bool(const Bytes& reply);
// GetValveState: "True" or "False", any case; anything else (legacy read it
// as closed) is a Protocol error.
Result<bool> decode_valve_state(const Bytes& reply);
// Bare CSV of floats, one per requested name in the same order; returns the
// values paired with `names`. Count mismatch is a Protocol error.
Result<Pairs> decode_named_values(const Bytes& reply, std::span<const std::string> names);
// GetData, tagged: "tag,value,tag,value,...". "Error: no data" is a Protocol
// error with code kNoData. Tags are returned exactly as
// sent (case included). An empty body, an unpaired or empty field, a value
// that is not a finite number and a repeated tag are each a Protocol error:
// a ruling pending a bench capture, so that no value is ever attributed to a
// detector by guesswork.
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

// Replies end with "\n", as on a KeepAlive connection.
Bytes encode_ok();                             // "OK"
Bytes encode_number(double v);                 // format_number(v)
Bytes encode_bool(bool v);                     // "True" / "False"
Bytes encode_error(std::string_view message);  // "Error: <message>"
Bytes encode_describe(const ServerInfo& server);  // one line of JSON
// Tagged GetData reply: "tag,value,tag,value,...".
Bytes encode_data(std::span<const std::pair<std::string, double>> pairs);
// `text` as a reply line, unchanged: for replies no encoder here would produce.
Bytes encode_line(std::string_view text);

}  // namespace pychron::codec::qtegra
