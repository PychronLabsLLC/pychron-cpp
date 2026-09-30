#pragma once

// Isotopx NGX ASCII protocol. See CONVENTIONS.md.
//
// Framing: every message, both directions, ends with "#\n".
//
//   host -> "Login user,password#\n"      login
//   host -> "SetMass 39.9624#\n"          set_mass / "GetMass#\n"  get_mass
//   host -> "StartAcq 1#\n" / "StopAcq#\n" / "SetAcqPeriod 1#\n" / "SAB#\n"
//   host -> "SSO YF,1.5#\n"               set source param / "GSO YF#\n"
//   NGX  -> "OK#\n"  |  "OK,39.9624#\n"  |  "OK,1.5,1.48#\n" (setpoint,readback)
//   NGX  -> "E03,busy#\n"                 any rejected command (Exx -> ErrorKind)
//
// Asynchronous event lines interleave with command replies:
//
//   "#EVENT:ACQ,<seq>,<ts_s>,<v_n>,...,<v_1>#\n"     integrated cycle
//   "#EVENT:ACQ.B,<seq>,<ts_s>,<v_n>,...,<v_1>#\n"   same layout, baseline block
//
// `ts_s` is instrument time in seconds; values arrive in reverse detector
// order and are returned here in channel order (channel 0 first). Units are
// whatever the instrument reports. Channel binding is the driver's job.
// Demultiplexer separates replies from events on a byte stream.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::ngx {

inline constexpr std::string_view kTerminator = "#\n";
inline constexpr std::string_view kEventPrefix = "#EVENT:";

// --- host side --------------------------------------------------------------

Result<Command> login(std::string_view user, std::string_view password);
Result<Command> set_mass(double mass);
Command get_mass();
Result<Command> start_acq(int count = 1);
Command stop_acq();
// Integration period in whole seconds (NGX snaps to 1 s multiples).
Result<Command> set_acq_period(int seconds);
Command sab();

// Source parameters, canonical names mirror devices::SourceParam.
enum class Param {
  HV, TrapCurrent, TrapVoltage, Emission, ElectronEnergy, IonRepeller, ExtractionLens,
  ExtractionFocus, ExtractionSymmetry, YSymmetry, ZSymmetry, ZFocus, HorizontalSymmetry,
  Flatapole, RotationQuad, PoleN, PoleS, ESAPlus, ESAMinus
};

// Wire mnemonic ("YF", ...). nullopt for params the NGX does not expose.
std::optional<std::string_view> mnemonic(Param p) noexcept;
std::optional<Param> param_from_mnemonic(std::string_view name) noexcept;

// "SSO <mnemonic>,<value>#\n" / "GSO <mnemonic>#\n". Config error for an
// unsupported param.
Result<Command> set_source_param(Param p, double value);
Result<Command> get_source_param(Param p);
// Vendor-named extra (Custom param); name must be [A-Za-z0-9_.]+.
Result<Command> set_source_param(std::string_view vendor_name, double value);
Result<Command> get_source_param(std::string_view vendor_name);

// --- errors -----------------------------------------------------------------

// Exx code -> ErrorKind: E04 not logged in -> NotConnected, E05 range/E02
// bad argument -> Config, E07 interlock -> Interlock, E08 timeout -> Timeout,
// anything else -> Protocol.
ErrorKind error_kind(int code) noexcept;
std::string_view error_text(int code) noexcept;

// --- decoders (complete message, including terminator) ------------------------

struct Readback {
  double setpoint = 0.0;
  std::optional<double> actual;
};

Result<void> decode_ok(const Bytes& reply);
Result<double> decode_mass(const Bytes& reply);
Result<Readback> decode_source_param(const Bytes& reply);

struct AcqFrame {
  std::uint64_t seq = 0;
  double ts_s = 0.0;           // instrument time, seconds
  bool baseline = false;       // ACQ.B
  std::vector<double> values;  // channel order
};
Result<AcqFrame> decode_acq_event(const Bytes& line);

// --- demultiplexer ------------------------------------------------------------

struct Reply { Bytes raw; };                 // command reply, terminator included
struct OtherEvent { std::string name; std::string body; };
using Message = std::variant<Reply, AcqFrame, OtherEvent>;

// Accumulates stream bytes and yields whole messages. Holds only its buffer.
class Demultiplexer {
 public:
  // Each complete line -> one entry; a malformed event is an error entry and
  // does not stop later lines.
  std::vector<Result<Message>> feed(const Bytes& chunk);
  std::size_t pending() const noexcept { return buffer_.size(); }
  void clear() noexcept { buffer_.clear(); }

 private:
  std::string buffer_;
};

}  // namespace pychron::codec::ngx
