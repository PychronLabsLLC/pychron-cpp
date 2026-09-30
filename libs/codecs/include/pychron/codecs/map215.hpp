#pragma once

// MAP-215 style serial magnet DAC. See CONVENTIONS.md.
//
//   host -> "B<range>."   select the output range (0..9)       (select_range)
//   host -> "W<code>."    write a DAC code (0..65535)          (write_code)
//
// The DAC is write-only: it never answers, so both commands are write-only
// and a positioner can only report what it last wrote. A write acts on the
// most recently selected range. Volts <-> code scaling over the configured
// full scale is the driver's job (to_code/to_volts are provided as helpers).

#include <cstdint>

#include "pychron/codecs/codec.hpp"

namespace pychron::codec::map215 {

inline constexpr int kMinRange = 0;
inline constexpr int kMaxRange = 9;
inline constexpr std::int64_t kMaxCode = 65535;
inline constexpr char kTerminator = '.';

// --- host side --------------------------------------------------------------

// "B<range>."; Config error for range outside kMinRange..kMaxRange.
Result<Command> select_range(int range);
// "W<code>."; Config error for code outside 0..kMaxCode.
Result<Command> write_code(std::int64_t code);

// Nearest code for `volts` on a 0..full_scale output; Config error when
// full_scale is not positive or volts falls outside 0..full_scale.
Result<std::int64_t> to_code(double volts, double full_scale);
// Output voltage of `code` on a 0..full_scale output.
double to_volts(std::int64_t code, double full_scale) noexcept;

// --- device side, for simulation hooks ---------------------------------------

struct Request {
  enum class Kind { SelectRange, Write };
  Kind kind = Kind::Write;
  std::int64_t value = 0;  // range or code

  friend bool operator==(const Request&, const Request&) = default;
};

// Parses one host transmission. Protocol error for anything else (including
// out-of-range values, which the DAC ignores).
Result<Request> decode_request(const Bytes& tx);

}  // namespace pychron::codec::map215
