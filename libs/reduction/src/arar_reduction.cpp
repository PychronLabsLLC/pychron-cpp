// Ar-Ar reduction step functions (spec 6) on the shared kernels (spec 8.1).
#include "pychron/reduction/arar_reduction.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include "arar_kernels.hpp"

namespace pychron::reduction {

namespace {

std::string fmt_g(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

// A constant read by a step function must be a valid variable (finite value,
// finite sigma >= 0) before it is minted.
Result<UFloat> mint(const Measured& m, std::string_view tag) {
  if (!std::isfinite(m.value) || !std::isfinite(m.error) || m.error < 0.0) {
    return fail(ErrorKind::Config, "reduction: constant " + std::string(tag) +
                                       " must have a finite value and a finite sigma >= 0, got " +
                                       fmt_g(m.value) + " +- " + fmt_g(m.error));
  }
  return UFloat::variable(m.value, m.error, tag);
}

}  // namespace

// ---- 3.1 Isotope arithmetic -----------------------------------------------

// E1. Without include_baseline_error legacy subtracts nominal_value(baseline),
// keeping the intercept's variables and dropping the baseline's.
// legacy:processing/isotope.py:715-736
UFloat baseline_corrected(const IsotopeSignal& s) {
  if (s.include_baseline_error) return s.intercept - s.baseline;
  return s.intercept - kernels::nominal(s.baseline);
}

// E2. The legacy background term (isotope.py:852-853) is always zero on the
// DVC path and is not ported (spec 3.1).
// legacy:processing/isotope.py:848-854
UFloat non_detector_corrected(const IsotopeSignal& s) {
  UFloat v = baseline_corrected(s);
  if (s.correct_for_blank) v = v - s.blank;
  return v;
}

// E3: (nd * D) * IC, the legacy get_disc_corrected_value then get_intensity
// order. An IC factor of 0 is used as is (spec Q18).
// legacy:processing/isotope.py:820-835
UFloat corrected_intensity(const IsotopeSignal& s) {
  return non_detector_corrected(s) * s.discrimination * s.ic_factor;
}

// E4. legacy:processing/argon_calculations.py:363-372
std::array<UFloat, 5> abundance_sensitivity_correction(const std::array<UFloat, 5>& s,
                                                       double alpha) {
  return kernels::abundance_sensitivity(s, alpha);
}

// E5 (D4). legacy:processing/deadtime.py:54-55 (formula); the legacy tool's
// 6240 factor (deadtime.py:62) is replaced by kFaToCountsPerSecond.
Result<UFloat> deadtime_correct(const UFloat& signal_fa, double tau_s, double fa_to_cps) {
  if (!std::isfinite(tau_s) || tau_s < 0.0) {
    return fail(ErrorKind::Config,
                "reduction: deadtime tau must be finite and >= 0, got " + std::to_string(tau_s));
  }
  if (!std::isfinite(fa_to_cps) || !(fa_to_cps > 0.0)) {
    return fail(ErrorKind::Config, "reduction: deadtime fA to counts/s factor must be finite "
                                   "and > 0, got " +
                                       std::to_string(fa_to_cps));
  }
  const double n_tau = signal_fa.nominal() * fa_to_cps * tau_s;
  if (!(1.0 - n_tau > 0.0)) {
    return fail(ErrorKind::Config,
                "reduction: deadtime saturated, 1 - n*tau <= 0 (n*tau = " +
                    std::to_string(n_tau) + ")");
  }
  return kernels::deadtime(signal_fa, tau_s, fa_to_cps);
}

Result<UFloat> deadtime_corrected_intercept(const IsotopeSignal& s, double fa_to_cps) {
  if (!s.deadtime_tau_s) return s.intercept;
  return deadtime_correct(s.intercept, *s.deadtime_tau_s, fa_to_cps);
}

// ---- 3.3 Interference corrections -----------------------------------------

// E9-E11. legacy:processing/argon_calculations.py:375-426
InterferenceComponents interference_corrections(const UFloat& a39, const UFloat& a37,
                                                const ProductionVariables& p,
                                                const InterferenceOptions& o,
                                                std::vector<Diagnostic>* diagnostics) {
  const kernels::InterferenceRatios<UFloat> r{p.k3739, p.k3839, p.ca3937, p.ca3837, p.ca3637};
  // :410, :416-417: a per-analysis value wins when truthy; otherwise Fixed mode
  // takes the constants value and Normal mode runs E9.
  const UFloat* fixed = nullptr;
  if (o.fixed_k3739 && kernels::truthy(*o.fixed_k3739)) {
    fixed = &*o.fixed_k3739;
  } else if (o.mode == K3739Mode::Fixed) {
    fixed = &o.constants_fixed_k3739;
  }
  const kernels::Interference<UFloat> k =
      kernels::interference(a39, a37, r, fixed, o.allow_negative_ca_correction);
  if (diagnostics != nullptr) {
    if (k.fixed_zero_ca3937) diagnostics->push_back(Diagnostic::FixedK3739ZeroCa3937);
    if (k.ca_clamped) diagnostics->push_back(Diagnostic::CaClampedToZero);
  }
  return {k.k37, k.k38, k.k39, k.ca36, k.ca37, k.ca38, k.ca39};
}

// ---- 3.4 Atmospheric, chlorine, cosmogenic ------------------------------

// E12. Spec Q1: lambda_Cl36 and the atm4036 / atm4038 inside atm3836 are
// fresh variables per call, as each legacy ArArConstants property read is
// (arar_constants.py:225-231). Legacy then re-wraps atm3836 as one variable
// tagged "atm3836" (argon_calculations.py:470-479); the spec keeps the two
// constituent variables instead, which gives the same sigma to rounding.
// legacy:processing/argon_calculations.py:468-487
Result<AtmosphericComponents> atmospheric_components(const UFloat& a38, const UFloat& a36,
                                                     const UFloat& k38, const UFloat& ca38,
                                                     const UFloat& ca36, double decay_days,
                                                     const UFloat& cl3638,
                                                     const ReductionConstants& c) {
  if (!std::isfinite(decay_days)) {
    return fail(ErrorKind::Config,
                "reduction: atmospheric decay_days must be finite, got " + fmt_g(decay_days));
  }
  const Result<UFloat> lcl = mint(c.lambda_cl36, "lambda_Cl36");
  if (!lcl) return fail(lcl.error());
  const Result<UFloat> atm4036 = mint(c.atm4036, "atm4036");
  if (!atm4036) return fail(atm4036.error());
  const Result<UFloat> atm4038 = mint(c.atm4038, "atm4038");
  if (!atm4038) return fail(atm4038.error());
  const UFloat r3836 = *atm4036 / *atm4038;
  const kernels::Atmospheric<UFloat> a =
      kernels::atmospheric(a38, a36, k38, ca38, ca36, decay_days, cl3638, *lcl, r3836);
  if (a.singular) {
    return fail(ErrorKind::Config,
                "reduction: atmospheric zero divisor, 1 - m*atm3836 == 0 (Cl3638 = " +
                    fmt_g(cl3638.nominal()) + ", decay_days = " + fmt_g(decay_days) + ")");
  }
  return AtmosphericComponents{a.atm36, a.atm38, a.cl36, a.cl38};
}

// E13. rs and rc are fresh variables per call (arar_constants.py:242-246).
// legacy:processing/argon_calculations.py:490-513
Result<CosmogenicComponents> cosmogenic_components(const UFloat& c36, const UFloat& c38,
                                                   const CosmogenicRatios& r) {
  const Result<UFloat> rs = mint(r.solar3836, "solar3836");
  if (!rs) return fail(rs.error());
  const Result<UFloat> rc = mint(r.cosmo3836, "cosmo3836");
  if (!rc) return fail(rc.error());
  const kernels::Cosmogenic<UFloat> k = kernels::cosmogenic(c36, c38, *rs, *rc);
  if (k.singular) {
    return fail(ErrorKind::Config,
                "reduction: cosmogenic zero divisor (c36 = " + fmt_g(c36.nominal()) +
                    ", cosmo3836 - solar3836 = " + fmt_g(rc->nominal() - rs->nominal()) + ")");
  }
  return CosmogenicComponents{k.cosmo36, k.cosmo38, k.noncosmo36, k.noncosmo38};
}

}  // namespace pychron::reduction
