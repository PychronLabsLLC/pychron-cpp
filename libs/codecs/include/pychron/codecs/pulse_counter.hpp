#pragma once

// Serial pulse counter (ion counting multiplier front end). See
// CONVENTIONS.md.
//
//   host    -> "R\r"              read and reset every channel     (read_counts)
//   counter -> "<n0>[,<n1>...]\r"  counts since the previous read   (decode_counts)
//   counter -> "E <message>\r"     rejected command                 (Protocol error)
//
// Counts are raw pulses; rate and dead-time correction are the host
// integrator's job.

#include <cstdint>
#include <string_view>
#include <vector>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::pulse_counter {

inline constexpr std::string_view kTerminator = "\r";

// --- host side --------------------------------------------------------------

Command read_counts();

// Exactly `channels` non-negative integer counts. Anything else, including an
// "E ..." reply, is a Protocol error.
Result<std::vector<std::uint64_t>> decode_counts(std::size_t channels, const Bytes& reply);

// --- counter side, for simulation hooks ---------------------------------------

// True when `tx` is a read_counts() request.
bool is_read_request(const Bytes& tx);
Bytes encode_counts(const std::vector<std::uint64_t>& counts);
Bytes encode_error(std::string_view message);

}  // namespace pychron::codec::pulse_counter
