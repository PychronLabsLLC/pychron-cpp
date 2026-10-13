#pragma once

// The gases the simulated extraction line holds (spec section 3.1).
//
// Six species, fixed: the five argon isotopes the spectrometer measures, and
// `Active`, everything a getter removes (N2, O2, H2O, CO2) as one bulk gas of
// mass 28. `Active` is there so that gauges read sensibly: air is about 1 %
// argon, and a line that has taken an air shot and not yet gettered it must
// show it.
//
// A `Composition` is one number per species. What the number is belongs to
// whoever holds it: a partial pressure (mbar), an amount (mbar L), a rate
// (mbar L / s, or 1 / s), or a ratio to Ar36 as in `air_ratios()`. Species do
// not interact, so everything done to a composition is done entry by entry.

#include <array>
#include <cstddef>
#include <string_view>

namespace pychron::sim {

enum class Species { Ar36, Ar37, Ar38, Ar39, Ar40, Active };

inline constexpr std::size_t kSpeciesCount = 6;

inline constexpr std::array<double, kSpeciesCount> kSpeciesMass{35.968, 36.967, 37.963, 38.964, 39.962, 28.0};

inline constexpr std::array<std::string_view, kSpeciesCount> kSpeciesName{"Ar36", "Ar37", "Ar38",
                                                                          "Ar39", "Ar40", "active"};

// Per species; meaning set by the user (mbar, mbar L, mbar L / s).
using Composition = std::array<double, kSpeciesCount>;

// Where a species sits in a Composition and in the tables above.
constexpr std::size_t index(Species species) noexcept { return static_cast<std::size_t>(species); }

constexpr double total(const Composition& c) noexcept {
  double sum = 0.0;
  for (const double value : c) sum += value;
  return sum;
}

constexpr Composition scaled(const Composition& c, double factor) noexcept {
  Composition out{};
  for (std::size_t i = 0; i < kSpeciesCount; ++i) out[i] = c[i] * factor;
  return out;
}

// Ratios to Ar36 (Ar36 == 1).
//
// Air: Ar40 298.56 and Ar38 0.1885 (Lee et al. 2006), no Ar37 or Ar39, and
// active gas 106 times the argon sum (argon is 0.934 % of air by volume).
constexpr Composition air_ratios() noexcept {
  Composition c{};
  c[index(Species::Ar36)] = 1.0;
  c[index(Species::Ar38)] = 0.1885;
  c[index(Species::Ar40)] = 298.56;
  c[index(Species::Active)] = 106 * (c[index(Species::Ar36)] + c[index(Species::Ar38)] + c[index(Species::Ar40)]);
  return c;
}

// A cocktail: air argon with Ar39 20 and Ar37 0.5 added, and no active gas.
constexpr Composition cocktail_ratios() noexcept {
  Composition c{};
  c[index(Species::Ar36)] = 1.0;
  c[index(Species::Ar37)] = 0.5;
  c[index(Species::Ar38)] = 0.1885;
  c[index(Species::Ar39)] = 20.0;
  c[index(Species::Ar40)] = 298.56;
  return c;
}

// The composition with these ratios whose Ar40 entry is `ar40`. Ratios with
// no Ar40 in them have no such composition: the result is then all zero.
constexpr Composition with_ar40(const Composition& ratios, double ar40) noexcept {
  const double reference = ratios[index(Species::Ar40)];
  if (!(reference > 0.0)) return Composition{};
  Composition out = scaled(ratios, ar40 / reference);
  out[index(Species::Ar40)] = ar40;  // exactly, not to a rounding of x * (y / x)
  return out;
}

}  // namespace pychron::sim
