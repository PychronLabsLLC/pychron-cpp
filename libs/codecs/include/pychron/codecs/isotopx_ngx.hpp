#pragma once

// Isotopx NGX ASCII protocol. See CONVENTIONS.md.
//
// Ground truth is pychron's production Python driver:
//   NC = pychron/hardware/isotopx_spectrometer_controller.py
//   SP = pychron/spectrometer/isotopx/spectrometer/ngx.py
//   MG = pychron/spectrometer/isotopx/magnet/ngx.py
//   SR = pychron/spectrometer/isotopx/source/ngx.py
//   IX = pychron/spectrometer/isotopx/__init__.py
//   GA = pychron/hardware/actuators/ngx_gp_actuator.py
//
// Framing: replies and event lines end with "#\r\n" (NC NGX_TERMINATOR, SP
// readline("#\r\n"), LineDemultiplexer). The send terminator is the
// communicator's configured write_terminator in Python (unverified: not
// determinable from pychron Python; the class default is "\r"). Every encoder
// takes it as a parameter, default "#\r\n".
//
//   host -> "Login user,password"                   (NC:147)
//   host -> "GETMASS"                               (MG:33)
//   host -> "SetMass 39.9624,500[,deflect]"         mass, settling ms (MG:59)
//   host -> "StartAcq 10,NOM"                       int(integration s), rcs id (SP)
//   host -> "StopAcq" / "SetAcqPeriod 1000" / "SAB 0|1"
//   host -> "SSO IE, 4500.0" / "GSO IE"             note the space after ',' (SR:29)
//   host -> "SetSourceOutput YF,1.5" / "GetSourceOutput YF"   (SP)
//   host -> "OpenValve 3" / "CloseValve 3" / "GetValveStatus 3" (GA, NC:33)
//   NGX  -> "E00"                    success (NC VALVE_RESPONSES, simulator)
//   NGX  -> "39.9624"                GETMASS: bare float
//   NGX  -> "4500,4499.8"            GSO / GetSourceOutput: setpoint,readback
//   NGX  -> "OPEN" | "CLOSED"        GetValveStatus
//   NGX  -> "E41"                    rejected command, bare Exx (IX ERRORS);
//                                    an optional ",text" suffix is tolerated
//
// Asynchronous event lines interleave with command replies (SP
// _parse_acquisition_event):
//
//   "#EVENT:ACQ,<rcs_id>,<f2>,<f3>,HH:MM:SS.ffffff,<v_n>,...,<v_1>#\r\n"
//   "#EVENT:ACQ.B,<rcs_id>,..."          same layout, baseline (buffered) block
//
// The clock time has no date (Python combines it with today). Values arrive
// in reverse detector order -- Python zips them with detectors[::-1] -- and
// are returned here in channel order (channel 0 first). Units are whatever
// the instrument reports. Channel binding is the driver's job.
//
// Driver notes (no codec behaviour; recorded for the NGX driver):
//  - Python reads one unsolicited banner line after connecting and only then
//    sends Login (NC:143-149); it re-sends Login on every reconnect and
//    consumes that reply (NC:160-178).
//  - Without demultiplexing an event line can arrive in place of a valve
//    reply, so Python retries valve commands up to 3 times until the reply is
//    one of E00/OPEN/CLOSED (NC:67-86). With the Demultiplexer below, replies
//    and events are routed separately.
//  - Python's actuator re-issues GetValveStatus when it reads "E00" in place
//    of a status (GA:66-80); decode_valve_status reports that as Protocol.
//  - Python matches ACQ events against its own rcs id ("#EVENT:ACQ,NOM");
//    filtering on AcqFrame::rcs_id is the driver's job.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::ngx {

// Reply and event line terminator (NC NGX_TERMINATOR).
inline constexpr std::string_view kTerminator = "#\r\n";
// unverified: not determinable from pychron Python. Python appends the
// communicator's configured write_terminator (EC:639, NC:92/172); "#\r\n" is
// inferred from the NGX login/payload code paths.
inline constexpr std::string_view kDefaultSendTerminator = "#\r\n";
// Lines starting with this are events (LineDemultiplexer event_prefix).
inline constexpr std::string_view kEventPrefix = "#EVENT";
inline constexpr std::string_view kDefaultRcsId = "NOM";  // SP rcs_id

// --- host side --------------------------------------------------------------

Result<Command> login(std::string_view user, std::string_view password,
                      std::string_view send_terminator = kDefaultSendTerminator);
Command get_mass(std::string_view send_terminator = kDefaultSendTerminator);
// "SetMass {mass},{delay_ms}[,deflect]". delay_ms is the settling time in ms
// (MG: int(settling_time * 1000), see settling_delay_ms).
Result<Command> set_mass(double mass, int delay_ms, bool deflect = false,
                         std::string_view send_terminator = kDefaultSendTerminator);
// Python's int(settling_time * 1000): truncation toward zero.
Result<int> settling_delay_ms(double settling_time_s);
// "StartAcq {int(integration_time_s)},{rcs_id}". Truncated like Python int();
// a count below 1 is a Config error (unverified: not determinable from
// pychron Python whether 0 is accepted).
Result<Command> start_acq(double integration_time_s, std::string_view rcs_id = kDefaultRcsId,
                          std::string_view send_terminator = kDefaultSendTerminator);
Command stop_acq(std::string_view send_terminator = kDefaultSendTerminator);
// Acquisition period in milliseconds. Python always sends "SetAcqPeriod 1000"
// with a comment calling it 1 s (SP set_integration_time); ms units are
// unverified: not determinable from pychron Python beyond that comment.
Result<Command> set_acq_period(int period_ms,
                               std::string_view send_terminator = kDefaultSendTerminator);
// Acquisition buffer flag: "SAB 1" / "SAB 0" (NC set_acquisition_buffer).
Command sab(bool enabled, std::string_view send_terminator = kDefaultSendTerminator);

// Source parameters, exactly pychron's SOURCE_CONTROL_PARAMETERS (IX:16-32).
enum class Param {
  IonEnergy, YFocus, YBias, ZFocus, ZBias, ElectronEnergy, IonRepeller, TrapVoltage,
  FilamentCurrent, FilamentVoltage, TrapCurrent, EmissionCurrent, ConfinementVoltage,
  ESAPlus, ESAMinus
};

// Wire mnemonic ("IE", "ESA+", ...).
std::string_view mnemonic(Param p) noexcept;
std::optional<Param> param_from_mnemonic(std::string_view mnemonic) noexcept;
// pychron's parameter name ("IonEnergy", "ESA+Plate", ...).
std::string_view name(Param p) noexcept;
std::optional<Param> param_from_name(std::string_view name) noexcept;

// "SSO <mnemonic>, <value>" / "GSO <mnemonic>" (SR). SSO has a space after
// the comma, as in Python.
Result<Command> set_source_param(Param p, double value,
                                 std::string_view send_terminator = kDefaultSendTerminator);
Result<Command> get_source_param(Param p, std::string_view send_terminator = kDefaultSendTerminator);
// Any name matching [A-Za-z0-9_.+-]+.
Result<Command> set_source_param(std::string_view vendor_name, double value,
                                 std::string_view send_terminator = kDefaultSendTerminator);
Result<Command> get_source_param(std::string_view vendor_name,
                                 std::string_view send_terminator = kDefaultSendTerminator);

// "SetSourceOutput {name},{value}" (SP set_source_parameter, no space) and
// "GetSourceOutput {key}" (SP read_parameter_word, key is the mnemonic).
Result<Command> set_source_output(std::string_view name, double value,
                                  std::string_view send_terminator = kDefaultSendTerminator);
Result<Command> get_source_output(std::string_view key,
                                  std::string_view send_terminator = kDefaultSendTerminator);
Result<Command> get_source_output(Param p, std::string_view send_terminator = kDefaultSendTerminator);

// Valves (GA). Address is the switch address as configured; no spaces,
// commas or '#'. Replies: decode_ok (E00) / decode_valve_status.
Result<Command> open_valve(std::string_view address,
                           std::string_view send_terminator = kDefaultSendTerminator);
Result<Command> close_valve(std::string_view address,
                            std::string_view send_terminator = kDefaultSendTerminator);
Result<Command> get_valve_status(std::string_view address,
                                 std::string_view send_terminator = kDefaultSendTerminator);

// Python str(float): shortest round-trip digits, fixed for 1e-4 <= |v| < 1e16
// (always with a fractional part, "4500.0"), otherwise "d.ddde+XX".
std::string format_float(double v);

// --- errors -----------------------------------------------------------------

// Exx code -> ErrorKind (IX ERRORS). Invalid param/range/mnemonic/missing ->
// Config; aborted -> Cancelled; hardware missing/fault, busy, not available
// -> Io; timeout -> Timeout; access denied -> NotConnected; rest -> Protocol.
ErrorKind error_kind(int code) noexcept;
// Python's name, e.g. "ERR_BUSY"; "ERR_UNKNOWN" for unlisted codes.
std::string_view error_text(int code) noexcept;

// --- decoders (complete message, including terminator) ------------------------

struct Readback {
  double setpoint = 0.0;
  double actual = 0.0;
};

Result<void> decode_ok(const Bytes& reply);                   // "E00"
Result<double> decode_mass(const Bytes& reply);               // bare float
Result<Readback> decode_source_param(const Bytes& reply);     // "setpoint,readback"
Result<bool> decode_valve_status(const Bytes& reply);         // OPEN -> true

// Instrument wall-clock time of day, "%H:%M:%S.%f" (no date on the wire).
struct ClockTime {
  int hour = 0;
  int minute = 0;
  int second = 0;
  int microsecond = 0;

  std::int64_t microseconds_since_midnight() const noexcept {
    return ((static_cast<std::int64_t>(hour) * 60 + minute) * 60 + second) * 1000000 + microsecond;
  }
  friend bool operator==(const ClockTime&, const ClockTime&) = default;
};

struct AcqFrame {
  bool baseline = false;       // ACQ.B
  std::string rcs_id;          // field 1
  std::string field2;          // meaning unknown; raw
  std::string field3;          // meaning unknown; raw
  ClockTime time;              // field 4
  std::vector<double> values;  // fields 5.., channel order (wire is reversed)
};
Result<AcqFrame> decode_acq_event(const Bytes& line);

// --- demultiplexer ------------------------------------------------------------

struct Reply { Bytes raw; };                 // command reply, terminator included
struct OtherEvent { std::string name; std::string body; };
using Message = std::variant<Reply, AcqFrame, OtherEvent>;

// Accumulates stream bytes and yields whole "#\r\n" lines; a terminator split
// across chunks is handled. Lines starting with "#EVENT" are events, anything
// else a reply (LineDemultiplexer). Holds only its buffer.
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
