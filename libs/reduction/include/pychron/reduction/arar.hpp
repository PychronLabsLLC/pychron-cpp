#pragma once

// Live Ar-Ar quantities for conditionals (conditionals spec section 5): age,
// K/Ca, radiogenic yield and the corrected argon components, from corrected
// 36-40 intensities and per-irradiation constants. Pure functions on the
// double instantiation of the shared kernels (reduction spec 8.1, 8.2), so the
// nominal values equal reduce()'s for the same inputs.
//
//   ca37, ca39, k39 (E9, or E10 when a fixed K3739 is selected; E11 clamp when
//   !allow_negative_ca_correction):
//     k39 = (Ar39 df39 - Ca3937 Ar37 df37) / (1 - K3739 Ca3937)
//     ca37 = Ar37 df37 - K3739 k39   ca39 = (39/37)Ca ca37   ca36 = (36/37)Ca ca37
//     k38 = K3839 k39                ca38 = Ca3837 ca37
//   atm36 (E12; m = 0 without chlorine):
//     m = Cl3638 lambda_Cl36 decay_days      r3836 = atm4036 / atm4038
//     atm36 = (Ar36 - ca36 - m (Ar38 - k38 - ca38)) / (1 - m r3836)
//     cl38 = Ar38 - r3836 atm36 - k38 - ca38  cl36 = cl38 m
//   atm40 = atm36 (40/36)atm   rad40 = Ar40 - atm40 - k39 (40/39)K   (E14)
//   radiogenic_yield = rad40_percent = rad40 / Ar40 * 100
//   F = rad40 / k39            age = lambda^-1 ln(1 + J F) * 1e-6  (Ma, E16)
//   kca = k39 / ca37 * kca_factor   cak = 1 / kca                  (E19)
//   kcl = k39 / cl38 * cl_k_factor  clk = 1 / kcl                  (E19, chlorine)
//
// With the default fields (K3739 = K3839 = Ca3837 = 0, no chlorine) this is
// the original live formula set: ca37 = Ar37 df37, k39 = Ar39 df39 - ca39,
// atm40 = (Ar36 - ca36) atm4036.

#include <map>
#include <optional>
#include <string>

#include "pychron/reduction/arar_types.hpp"

namespace pychron::reduction {

// E12 chlorine inputs for the live path. With these set and Ar38 present,
// compute_arar also emits cl36, kcl and clk.
struct LiveChlorine {
  double cl3638 = 0;            // production (36/38)Cl
  double lambda_cl36 = 6.308e-9;  // 1/day
  double decay_days = 0;        // days since irradiation
  double atm4038 = 1575.0;      // atm3836 = atm4036 / atm4038
  double cl_k_factor = 1.0;     // 1 / Cl_K (1 when missing or zero)
};

struct ArArConstants {
  double lambda_total = 5.543e-10;  // 1/yr, Steiger & Jaeger (1977)
  double atm4036 = 298.56;          // Lee et al. (2006); equals the Default preset (D5)
  double ca3637 = 0.0;              // production ratios: irradiation specific, default none
  double ca3937 = 0.0;
  double k4039 = 0.0;
  double j = 0.0;                   // required for age; 0 = no age
  double df37 = 1.0, df39 = 1.0;    // decay factors since irradiation
  double kca_factor = 1.0;          // K/Ca conversion (e.g. 0.5 for (39/37)K/Ca ratios)
  double k3739 = 0.0, k3839 = 0.0, ca3837 = 0.0;
  bool allow_negative_ca_correction = true;
  std::optional<LiveChlorine> chlorine;  // absent: no chlorine correction, no kcl/clk/cl36
  // E10 selection, as reduce(): a per-analysis value wins when nonzero; else
  // Fixed mode uses fixed_k3739; else Normal mode (E9).
  K3739Mode k3739_mode = K3739Mode::Normal;
  double fixed_k3739 = 0.0;
  std::optional<double> analysis_fixed_k3739;
};

struct ArArIntensities {
  std::optional<double> ar36, ar37, ar38, ar39, ar40;
};

// Every quantity computable from what is present, keyed by its conditional
// name (age, kca, cak, radiogenic_yield, rad40, rad40_percent, atm40, k39,
// ca37, ca39, ca36, and with chlorine kcl, clk, cl36). Omissions:
//   - With a fixed K3739 selected (E10) ca37/ca36/ca39 need Ar39 only (ca37
//     comes from Ar39 alone); otherwise (E9) they need Ar37, and Ar39 too when
//     K3739 != 0. k39 needs Ar39. An absent Ar37 or Ar39 enters the other
//     quantities as 0, and Ca components that cannot be computed enter as 0
//     (ca36 = ca38 = 0, k39 = k38 = 0).
//   - atm40 needs Ar36, and Ar38 too when chlorine is set and its correction
//     is not a no-op (m = Cl3638 lambda_Cl36 decay_days != 0); rad40 needs
//     atm40 and Ar40.
//   - radiogenic_yield needs Ar40 != 0; age needs k39 != 0, J > 0,
//     lambda_total > 0 and 1 + J F > 0; kca needs ca37 != 0 and k39 != 0,
//     cak kca != 0; kcl needs cl38 != 0 and k39 != 0, clk kcl != 0.
//   - an exactly singular E12 divisor (1 - m r3836 == 0) omits atm40 onward.
// No validation: non-finite input propagates (never an error, no sentinels).
std::map<std::string, double> compute_arar(const ArArIntensities& in, const ArArConstants& c);

// Live constants from the same sources as reduce() (spec 8.2): nominal values
// of the production ratios, atm4036, J and lambda_K (lambda_k_total when set
// and not 0 +- 0, else lambda_b + lambda_e), the decay factors, kca_factor =
// 1 / Ca_K (1 when missing or zero), the K3739 mode and constants value, and
// allow_negative_ca_correction. `chlorine` is set only when decay_days is
// given (Cl3638, lambda_Cl36, atm4038, cl_k_factor = 1 / Cl_K, 1 when missing
// or zero). Live ages are always in Ma.
ArArConstants to_live_constants(const ReductionConstants& c, const ProductionRatios& p,
                                const Flux& flux, const DecayFactors& df,
                                std::optional<double> decay_days = std::nullopt);

}  // namespace pychron::reduction
