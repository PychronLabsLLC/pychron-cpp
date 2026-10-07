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
//
// `keyed_poisson` is a count (a pulse counter's reading) by a method written
// out here, because std::poisson_distribution's is left to each standard
// library and the same seed gives other counts under libc++, libstdc++ and
// MSVC. Below a mean of 30 it is the multiplication method (Knuth 1969,
// 3.4.1): uniforms from the generator of (seed, key, tick) are multiplied
// until the product is at or below exp(-mean), and the count is the number
// of factors less one; that is exact. From 30 up it is the normal
// approximation with a continuity correction, the nearest integer to
// mean + sqrt(mean) * keyed_gauss(seed, key, tick), never below zero: the
// mean is the mean and the variance the mean plus a twelfth (the rounding),
// but the Poisson skew, 1 / sqrt(mean) (0.18 at 30, 0.03 at 1000), is left
// out, so the tails are symmetric where a true count's upper tail is the
// longer. The count is the same on every platform unless a product falls
// within a rounding error of exp(-mean), or the unrounded value within one
// of a half-integer: about one draw in 1e15.

#include <algorithm>
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

namespace keyed_noise_detail {

// Below this mean a count is drawn by multiplication, from it up by the
// normal approximation.
inline constexpr double kPoissonNormalFrom = 30.0;
// The largest count given: 2^62, which an int64 holds.
inline constexpr double kPoissonMost = 0x1.0p62;

}  // namespace keyed_noise_detail

// A Poisson count with this mean, the same on every platform for the same
// (seed, key, tick). mean <= 0 gives 0, and so does a mean that is not a
// number; a count is at most 2^62.
inline std::int64_t keyed_poisson(std::uint64_t seed, std::string_view key, std::int64_t tick, double mean) noexcept {
  if (!(mean > 0.0)) return 0;
  if (mean < keyed_noise_detail::kPoissonNormalFrom) {
    // Each factor is from (0, 1] and the limit is above exp(-30): the
    // product is there after about `mean` factors, a few dozen at most.
    std::uint64_t state = keyed_noise_detail::start(seed, key, tick);
    const double limit = std::exp(-mean);
    double product = 1.0;
    std::int64_t count = -1;
    do {
      product *= static_cast<double>((keyed_noise_detail::next(state) >> 11) + 1) * 0x1.0p-53;
      ++count;
    } while (product > limit);
    return count;
  }
  const double value = std::floor(mean + std::sqrt(mean) * keyed_gauss(seed, key, tick) + 0.5);
  if (!(value > 0.0)) return 0;
  return static_cast<std::int64_t>(std::min(value, keyed_noise_detail::kPoissonMost));
}

}  // namespace pychron::sim
