#pragma once

// Granville-Phillips Micro-Ion (Series 350 Micro-Ion Plus / 358) ASCII
// protocol in RS-485 addressed form. See CONVENTIONS.md.
//
//   host -> "#01DS IG\r"      read the ion gauge            (read_pressure)
//   gauge-> "*01 1.20E-06\r"  "*<addr> <value>"             (decode_pressure)
//   host -> "#01IG1 ON\r"     ion gauge filament on / OFF   (set_ion_gauge)
//   gauge-> "*01 PROGM OK\r"                                (decode_ok)
//   gauge-> "?01 SYNTX ER\r"  any rejected command          (Protocol error)
//
// Addresses are two hex digits (00..FF). Every reply echoes the address; a
// reply for another address is a Protocol error. The controller reports
// 9.90E+09 for a gauge that is off or unplugged. Some firmware ends replies
// with "\r\n"; the stray "\n" is tolerated at the start of the next reply.
// Values are in the controller's configured unit, never converted here.

#include <optional>
#include <string_view>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::microion {

inline constexpr int kMinAddress = 0x00;
inline constexpr int kMaxAddress = 0xFF;
inline constexpr std::string_view kTerminator = "\r";
// What the controller reports for a gauge that is off or has no sensor.
inline constexpr double kGaugeOff = 9.90e9;

// Sensor channels, numbered as the driver's `channels` config key uses them.
enum class Sensor { IonGauge = 1, ConvectronA = 2, ConvectronB = 3 };
inline constexpr int kFirstChannel = 1;
inline constexpr int kLastChannel = 3;

// Wire mnemonic: "IG", "CG1", "CG2".
std::string_view to_string(Sensor sensor) noexcept;

// --- host side --------------------------------------------------------------

// "#<aa>DS <IG|CG1|CG2>\r". Config error for an address outside 00..FF or a
// channel outside kFirstChannel..kLastChannel.
Result<Command> read_pressure(int address, int channel);
// "#<aa>IG1 ON\r" / "#<aa>IG1 OFF\r": ion gauge filament on or off.
Result<Command> set_ion_gauge(int address, bool on);

// "*<aa> <value>\r" from `address`. The gauge-off value and "?<aa> ..." error
// replies are Protocol errors.
Result<double> decode_pressure(int address, const Bytes& reply);
// "*<aa> PROGM OK\r" from `address`.
Result<void> decode_ok(int address, const Bytes& reply);

// --- controller side, for simulation hooks ------------------------------------

struct Request {
  enum class Kind { Pressure, IonGaugeOn, IonGaugeOff };
  Kind kind = Kind::Pressure;
  int address = 0;
  int channel = 0;  // Pressure only

  friend bool operator==(const Request&, const Request&) = default;
};

// Parses what a host wrote. Protocol error for anything the controller would
// answer with a syntax error (or ignore, when the address is unreadable).
Result<Request> decode_request(const Bytes& tx);
// The address a host transmission starts with ("#<aa>..."), even when the
// rest is unreadable; nullopt if there is none.
std::optional<int> addressee(const Bytes& tx);

// "*<aa> <d.ddE+dd>\r", the controller's own formatting.
Bytes encode_pressure(int address, double value);
Bytes encode_ok(int address);
// "?<aa> <message>\r", e.g. message "SYNTX ER".
Bytes encode_error(int address, std::string_view message);

}  // namespace pychron::codec::microion
