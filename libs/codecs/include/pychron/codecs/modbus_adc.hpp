#pragma once

// Multichannel ADC over Modbus TCP (legacy Faraday banks). See CONVENTIONS.md.
//
//   host -> MBAP{tid, 0, 6, unit} 04 <start:u16> <count:u16>   read input registers
//   adc  -> MBAP{tid, 0, 3+2n, unit} 04 <2n:u8> <reg:u16>*n
//   adc  -> MBAP{tid, 0, 3, unit} 84 <exception code>           rejected request
//
// Each ADC channel is one IEEE-754 float32 in two consecutive registers, high
// word first (the common "ABCD" layout). Values are volts as the ADC reports
// them; scaling to detector units belongs to the driver or engine.

#include <cstdint>
#include <vector>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::modbus_adc {

inline constexpr std::uint8_t kReadInputRegisters = 0x04;
inline constexpr std::uint8_t kExceptionFlag = 0x80;
inline constexpr std::uint8_t kIllegalAddress = 0x02;
inline constexpr std::uint16_t kMaxRegisters = 125;
inline constexpr int kRegistersPerChannel = 2;

// --- host side --------------------------------------------------------------

// Read `count` input registers from `start`. Config error for count outside
// 1..kMaxRegisters or a range past register 0xFFFF.
Result<Command> read_input_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t start,
                                     std::uint16_t count);
// `channels` float channels starting at register `start`.
Result<Command> read_channels(std::uint16_t tid, std::uint8_t unit, std::uint16_t start, std::size_t channels);

// The register values of a read reply matching `tid`, `unit` and `count`.
// Exception replies, mismatched ids and malformed frames are Protocol errors.
Result<std::vector<std::uint16_t>> decode_registers(std::uint16_t tid, std::uint8_t unit, std::uint16_t count,
                                                    const Bytes& reply);
// decode_registers() for `channels` floats, as doubles. Non-finite values are
// Protocol errors.
Result<std::vector<double>> decode_channels(std::uint16_t tid, std::uint8_t unit, std::size_t channels,
                                            const Bytes& reply);

// Float32 <-> register pair, high word first.
std::vector<std::uint16_t> to_registers(float value);
float to_float(std::uint16_t high, std::uint16_t low) noexcept;

// --- device side, for simulation hooks ---------------------------------------

struct Request {
  std::uint16_t tid = 0;
  std::uint8_t unit = 0;
  std::uint8_t function = 0;
  std::uint16_t start = 0;
  std::uint16_t count = 0;

  friend bool operator==(const Request&, const Request&) = default;
};

// A complete MBAP request frame. Protocol error if malformed.
Result<Request> decode_request(const Bytes& tx);
Bytes encode_registers(std::uint16_t tid, std::uint8_t unit, const std::vector<std::uint16_t>& registers);
Bytes encode_exception(std::uint16_t tid, std::uint8_t unit, std::uint8_t function, std::uint8_t code);

}  // namespace pychron::codec::modbus_adc
