#pragma once

// NCD ProXR relay controller, standard (non-API) command set.
//
// Every command is the prefix byte 254 followed by an opcode and optional
// argument; the board answers with exactly one byte:
//
//   254 49 <bank>   select bank 1..32          -> 'U' (0x55)
//   254 8+r         energize relay r (0..7)    -> 'U'
//   254 r           de-energize relay r        -> 'U'
//   254 16+r        relay r status             -> 0 or 1
//
// Relay commands act on the currently selected bank. A flat relay index
// 0..255 maps to bank index/8 + 1, relay index%8 (pychron's convention).
// Reference: pychron hardware/ncd/relay.py, NCD "ProXR Command Set" manual.

#include <cstdint>

#include "pychron/codecs/codec.hpp"
#include "pychron/core/error.hpp"
#include "pychron/transport/bytes.hpp"

namespace pychron::codec::proxr {

inline constexpr std::uint8_t kPrefix = 0xFE;
inline constexpr std::uint8_t kSelectBank = 49;
inline constexpr std::uint8_t kRelayOffBase = 0;
inline constexpr std::uint8_t kRelayOnBase = 8;
inline constexpr std::uint8_t kRelayStatusBase = 16;
inline constexpr std::uint8_t kAck = 0x55;  // 'U'

inline constexpr int kBanks = 32;
inline constexpr int kRelaysPerBank = 8;
inline constexpr std::int64_t kMaxIndex = kBanks * kRelaysPerBank - 1;

struct RelayAddress {
  int bank = 1;   // 1..32
  int relay = 0;  // 0..7 within the bank

  friend bool operator==(const RelayAddress&, const RelayAddress&) = default;
};

// Flat index 0..255 to bank/relay; Config error outside that range.
Result<RelayAddress> relay_address(std::int64_t index);

// Config error for bank outside 1..32 or relay outside 0..7.
Result<Command> select_bank(int bank);
Result<Command> relay_on(int relay);
Result<Command> relay_off(int relay);
Result<Command> read_relay(int relay);

// Reply to select_bank / relay_on / relay_off: exactly 'U'.
Result<void> decode_ack(const Bytes& reply);
// Reply to read_relay: true when the relay is energized.
Result<bool> decode_relay_state(const Bytes& reply);

}  // namespace pychron::codec::proxr
