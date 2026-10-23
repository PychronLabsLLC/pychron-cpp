// Parity bridge between the spec'd Ar-Ar reduction (arar_types.hpp, reduce())
// and the live path (pychron/reduction/arar.hpp, compute_arar): the live
// constants built from the inputs reduce() takes (spec 8.2).
#pragma once

#include <optional>

#include "pychron/arar/arar_types.hpp"
#include "pychron/reduction/arar.hpp"

namespace pychron::reduction {

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
