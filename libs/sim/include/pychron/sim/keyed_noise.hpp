#pragma once

// Keyed noise: random draws that are a function of what is being drawn, not
// of who drew before (lab simulator spec section 5.3).
//
// A simulated reading (a gauge's pressure, a detector's signal) carries
// noise. Drawn from one shared generator, the noise on a reading depends on
// how many readings anyone took before it: on polling rates and on which
// thread got there first. Here a draw is a hash of three things, a seed, a
// key naming the source (a volume, a detector) and a tick (the time of the
// reading, as a count), so the same reading has the same noise in every run
// and whatever else was read, and two readings of one source at one instant
// are one reading.
//
// The hash is FNV-1a over the key, folded with the seed and the tick through
// the splitmix64 finalizer (Steele, Lea and Flood 2014), and a draw is the
// first output of a splitmix64 generator started there; a Gaussian draw uses
// the first two. `keyed_bits` is fixed arithmetic on 64-bit integers: the
// same bits from every compiler and standard library, which <random>'s
// distributions do not promise. `keyed_gauss` takes those bits through
// std::log and std::cos, which maths libraries may round differently in the
// last place: the same draw on one platform, and to about one part in 1e16
// across platforms.

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string_view>

namespace pychron::sim {

namespace keyed_noise_detail {

inline constexpr std::uint64_t kGamma = 0x9e3779b97f4a7c15ULL;

constexpr std::uint64_t mix(std::uint64_t z) noexcept {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

// The next output of the splitmix64 generator whose state is `state`.
constexpr std::uint64_t next(std::uint64_t& state) noexcept { return mix(state += kGamma); }

// Where the generator of (seed, key, tick) starts.
constexpr std::uint64_t start(std::uint64_t seed, std::string_view key, std::int64_t tick) noexcept {
  std::uint64_t hash = 0xcbf29ce484222325ULL;  // FNV-1a
  for (const char c : key) hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
  return mix((mix((mix(seed + kGamma) ^ hash) + kGamma) ^ static_cast<std::uint64_t>(tick)) + kGamma);
}

}  // namespace keyed_noise_detail

// 64 uniform bits; the same (seed, key, tick) always gives the same.
constexpr std::uint64_t keyed_bits(std::uint64_t seed, std::string_view key, std::int64_t tick) noexcept {
  std::uint64_t state = keyed_noise_detail::start(seed, key, tick);
  return keyed_noise_detail::next(state);
}

// A draw from the normal distribution of mean 0 and sigma 1 (Box-Muller);
// finite, and the same for the same (seed, key, tick) (to the last place
// across maths libraries, see above).
inline double keyed_gauss(std::uint64_t seed, std::string_view key, std::int64_t tick) noexcept {
  std::uint64_t state = keyed_noise_detail::start(seed, key, tick);
  // 53 bits each: the radius from (0, 1], so its logarithm is finite; the
  // angle from [0, 1).
  const double radius = static_cast<double>((keyed_noise_detail::next(state) >> 11) + 1) * 0x1.0p-53;
  const double angle = static_cast<double>(keyed_noise_detail::next(state) >> 11) * 0x1.0p-53;
  return std::sqrt(-2.0 * std::log(radius)) * std::cos(2.0 * std::numbers::pi * angle);
}

}  // namespace pychron::sim
