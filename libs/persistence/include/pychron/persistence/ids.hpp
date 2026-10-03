#pragma once

// Identifiers and time for the DVC store (DVC schema spec, sections 1.3 P3/P4,
// 9.1). Every row id is a client-generated UUIDv7; change_seq is the only
// server-assigned number. All times are UTC with microsecond resolution.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#include "pychron/core/sha256.hpp"

namespace pychron::persistence {

class Uuid {
 public:
  using Bytes = std::array<std::uint8_t, 16>;

  constexpr Uuid() = default;  // nil
  constexpr explicit Uuid(const Bytes& bytes) : bytes_(bytes) {}

  // A fresh UUIDv7 from the process-wide generator (monotonic per process).
  static Uuid v7();
  // RFC 9562 name-based UUIDv5 (SHA-1 over namespace bytes then name):
  // deterministic, so re-deriving an id from the same inputs is idempotent.
  static Uuid v5(const Uuid& ns, std::string_view name);
  // Canonical 8-4-4-4-12 hex, either case; anything else is nullopt.
  static std::optional<Uuid> parse(std::string_view text);

  // Lower-case canonical form, the only form written to either engine.
  std::string str() const;
  bool is_nil() const noexcept;
  int version() const noexcept { return bytes_[6] >> 4; }
  const Bytes& bytes() const noexcept { return bytes_; }

  friend auto operator<=>(const Uuid&, const Uuid&) = default;

 private:
  Bytes bytes_{};
};

// RFC 9562 UUIDv7 with a monotonic 74-bit random field: within one
// millisecond each id is the previous one plus one, so ids from one generator
// sort in generation order even when the clock stalls or steps back.
class UuidV7Generator {
 public:
  using MillisClock = std::function<std::int64_t()>;  // Unix epoch milliseconds

  UuidV7Generator();
  UuidV7Generator(MillisClock clock, std::uint64_t seed);

  Uuid next();

 private:
  MillisClock clock_;
  std::mutex mutex_;
  std::mt19937_64 rng_;
  std::int64_t last_ms_ = -1;
  std::uint64_t rand_hi_ = 0;  // 12 bits (rand_a)
  std::uint64_t rand_lo_ = 0;  // 62 bits (rand_b)
};

using ChangeSeq = std::int64_t;

// Microseconds since the Unix epoch, UTC.
struct UtcTime {
  std::int64_t micros = 0;

  static UtcTime now();
  // "YYYY-MM-DDTHH:MM:SS[.f{1,6}]Z". Nothing else is accepted.
  static std::optional<UtcTime> parse(std::string_view iso);
  // Always "YYYY-MM-DDTHH:MM:SS.ffffffZ".
  std::string iso() const;

  friend auto operator<=>(const UtcTime&, const UtcTime&) = default;
};

}  // namespace pychron::persistence

template <>
struct std::hash<pychron::persistence::Uuid> {
  std::size_t operator()(const pychron::persistence::Uuid& u) const noexcept {
    std::size_t h = 0;
    for (auto b : u.bytes()) h = h * 131 + b;
    return h;
  }
};
