// Ar-Ar reduction step functions (spec 6) on the shared kernels (spec 8.1).
#include "pychron/reduction/arar_reduction.hpp"

#include <cmath>
#include <string>

#include "arar_kernels.hpp"

namespace pychron::reduction {

std::string_view to_string(Diagnostic d) noexcept {
  switch (d) {
    case Diagnostic::FUndefined: return "FUndefined";
    case Diagnostic::YieldUndefined: return "YieldUndefined";
    case Diagnostic::AgeUndefined: return "AgeUndefined";
    case Diagnostic::KCaUndefined: return "KCaUndefined";
    case Diagnostic::KClUndefined: return "KClUndefined";
    case Diagnostic::CaClampedToZero: return "CaClampedToZero";
    case Diagnostic::FixedK3739ZeroCa3937: return "FixedK3739ZeroCa3937";
    case Diagnostic::NonFiniteResult: return "NonFiniteResult";
  }
  return "";
}

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

}  // namespace pychron::reduction
