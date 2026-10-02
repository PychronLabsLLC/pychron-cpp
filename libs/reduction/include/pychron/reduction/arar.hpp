#pragma once

// Live Ar-Ar quantities for conditionals (conditionals spec section 5): age,
// K/Ca, radiogenic yield and the corrected argon components, from corrected
// 36-40 intensities and per-irradiation constants. Pure functions.
//
//   ca37 = Ar37 * df37            ca36 = (36/37)Ca * ca37    ca39 = (39/37)Ca * ca37
//   k39  = Ar39 * df39 - ca39     atm36 = Ar36 - ca36       atm40 = atm36 * (40/36)atm
//   rad40 = Ar40 - atm40 - (40/39)K * k39
//   radiogenic_yield = rad40_percent = 100 * rad40 / Ar40
//   F = rad40 / k39               age = ln(1 + J F) / lambda   (Ma)
//   kca = kca_factor * k39 / ca37 cak = 1 / kca
//
// Chlorine corrections are not modelled: kcl, clk and cl36 are never produced.

#include <map>
#include <optional>
#include <string>

namespace pychron::reduction {

struct ArArConstants {
  double lambda_total = 5.543e-10;  // 1/yr, Steiger & Jaeger (1977)
  double atm4036 = 298.56;          // Lee et al. (2006)
  double ca3637 = 0.0;              // production ratios: irradiation specific, default none
  double ca3937 = 0.0;
  double k4039 = 0.0;
  double j = 0.0;                   // required for age; 0 = no age
  double df37 = 1.0, df39 = 1.0;    // decay factors since irradiation
  double kca_factor = 1.0;          // K/Ca conversion (e.g. 0.5 for (39/37)K/Ca ratios)
};

struct ArArIntensities {
  std::optional<double> ar36, ar37, ar38, ar39, ar40;
};

// Every quantity computable from what is present, keyed by its conditional
// name (age, kca, cak, radiogenic_yield, rad40, rad40_percent, atm40, k39,
// ca37, ca39, ca36). Quantities that would divide by zero are omitted.
std::map<std::string, double> compute_arar(const ArArIntensities& in, const ArArConstants& c);

}  // namespace pychron::reduction
