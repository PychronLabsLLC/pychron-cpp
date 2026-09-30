#pragma once

// Pfeiffer MaxiGauge (TPG 256 A) mnemonic protocol. See CONVENTIONS.md.
//
// Every query is a two-step handshake on the wire:
//
//   host  -> "PR1\r\n"            mnemonic                 (read_pressure)
//   gauge -> "\x06\r\n"           ACK, or NAK "\x15\r\n"   (decode_ack)
//   host  -> "\x05"               ENQ                      (enquiry)
//   gauge -> "0,+1.2300E-08\r\n"  status,value             (decode_reading)
//
// The driver sequences the two exchanges; this codec only knows the bytes.
// Values are in the gauge's configured unit (UNI), never converted here.

#include <span>
#include <string_view>
#include <vector>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::maxigauge {

inline constexpr int kFirstChannel = 1;
inline constexpr int kLastChannel = 6;
inline constexpr int kChannelCount = kLastChannel - kFirstChannel + 1;
inline constexpr std::string_view kTerminator = "\r\n";
inline constexpr std::uint8_t kEnq = 0x05;
inline constexpr std::uint8_t kAck = 0x06;
inline constexpr std::uint8_t kNak = 0x15;

// Per-channel status digit preceding every reading.
enum class Status {
  Ok = 0,
  Underrange = 1,
  Overrange = 2,
  SensorError = 3,
  SensorOff = 4,
  NoSensor = 5,
  IdentificationError = 6,
};

std::string_view to_string(Status status) noexcept;

enum class Units { Mbar = 0, Torr = 1, Pascal = 2 };

// Config spelling: "mbar", "torr", "pa".
std::string_view to_string(Units units) noexcept;

struct Reading {
  Status status = Status::Ok;
  double value = 0.0;

  friend bool operator==(const Reading&, const Reading&) = default;
};

// --- host side: mnemonics (each answered by ACK/NAK) -----------------------

// "PR<channel>\r\n". Config error unless kFirstChannel <= channel <= kLastChannel.
Result<Command> read_pressure(int channel);
// "PRX\r\n": status and value of all six channels.
Command read_all_pressures();
// "UNI\r\n": the pressure unit the gauge reports in.
Command read_units();
// ENQ: asks the gauge to transmit the data for the last acknowledged mnemonic.
Command enquiry();

// --- host side: replies ----------------------------------------------------

// Ok for "\x06\r\n"; Protocol error for NAK or anything else.
Result<void> decode_ack(const Bytes& reply);
// "<status>,<value>\r\n".
Result<Reading> decode_reading(const Bytes& reply);
// The value of a reading the gauge vouches for. Ok, Underrange and Overrange
// carry a usable value (the range limit for the latter two); sensor faults
// are Protocol errors naming the status.
Result<double> pressure_value(const Reading& reading);
// decode_reading() then pressure_value().
Result<double> decode_pressure(const Bytes& reply);
// PRX reply: exactly kChannelCount "<status>,<value>" pairs, channel 1 first.
Result<std::vector<Reading>> decode_all_readings(const Bytes& reply);
// UNI reply: "<0|1|2>\r\n".
Result<Units> decode_units(const Bytes& reply);

// --- gauge side, for simulation hooks -------------------------------------

// A host transmission as the gauge understands it.
struct Request {
  enum class Kind { Pressure, AllPressures, Units, Enquiry };
  Kind kind = Kind::Enquiry;
  int channel = 0;  // Pressure only

  friend bool operator==(const Request&, const Request&) = default;
};

// Parses what a host wrote: a single ENQ byte, or a mnemonic terminated by
// "\r\n" or "\r". Protocol error for anything the gauge would NAK.
Result<Request> decode_request(const Bytes& tx);

Bytes encode_ack();
Bytes encode_nak();
// "<status>,<+d.dddd E+dd>\r\n", the gauge's own formatting.
Bytes encode_reading(const Reading& reading);
// PRX data line; `readings` should hold kChannelCount entries.
Bytes encode_all_readings(std::span<const Reading> readings);
Bytes encode_units(Units units);

}  // namespace pychron::codec::maxigauge
