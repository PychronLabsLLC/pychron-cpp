// Ar-Ar reduction step functions (spec 6) on the shared kernels (spec 8.1).
#include "pychron/reduction/arar_reduction.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

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

// atm4036 / atm4038 as `uncertainties` evaluates it for two independent
// variables: nominal a / b, std sqrt((sa / b)^2 + (a sb / b^2)^2), with the
// partials formed exactly as UFloat::operator/ forms them.
// legacy:processing/arar_constants.py:225-226
Result<Measured> atm3836(const ReductionConstants& c) {
  for (const auto& [m, name] : {std::pair{&c.atm4036, "atm4036"}, std::pair{&c.atm4038, "atm4038"}}) {
    if (!std::isfinite(m->value) || !std::isfinite(m->error) || m->error < 0.0) {
      return fail(ErrorKind::Config, "reduction: constant " + std::string(name) +
                                         " must have a finite value and a finite sigma >= 0, got " +
                                         fmt_g(m->value) + " +- " + fmt_g(m->error));
    }
  }
  const double a = c.atm4036.value;
  const double b = c.atm4038.value;
  const double da = 1.0 / b;
  const double db = -a / (b * b);
  const double ca = da * c.atm4036.error;
  const double cb = db * c.atm4038.error;
  const double variance = ca * ca + cb * cb;
  const double sigma = std::sqrt(variance);
  if (!std::isfinite(a / b) || !std::isfinite(sigma)) {
    return fail(ErrorKind::Config, "reduction: atm4036 / atm4038 is not finite (atm4038 = " +
                                       fmt_g(b) + ")");
  }
  return Measured{a / b, sigma};
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

// E12. Spec Q1: lambda_Cl36 and atm3836 are fresh variables per call. Legacy
// reads atm3836 as atm4036 / atm4038 (two fresh property reads,
// arar_constants.py:225-231) and re-wraps it as ONE fresh variable tagged
// "atm3836" with that ratio's nominal and std (argon_calculations.py:470-479);
// so does this. It is distinct from E14's trapped_4036.
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
  const Result<Measured> ratio = atm3836(c);
  if (!ratio) return fail(ratio.error());
  const UFloat r3836 = UFloat::variable(ratio->value, ratio->error, "atm3836");
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

// ---- 3.5 F and radiogenic yield -------------------------------------------

namespace {

bool finite(const UFloat& x) noexcept {
  return std::isfinite(x.nominal()) && std::isfinite(x.std_dev());
}

}  // namespace

// E9-E15. legacy:processing/argon_calculations.py:516-591 (calculate_f), one
// pass: the legacy second calc_f with zero-error ratios (:585-589) is replaced
// by std_dev_excluding over the interference-ratio ids (spec E15).
Result<FResult> calculate_f(const std::array<UFloat, 5>& n, double decay_days,
                            const ProductionVariables& p, const ReductionConstants& c,
                            std::optional<Measured> fixed_k3739) {
  for (const ArgonIsotope iso : kArgonKeys) {
    if (!std::isfinite(n[index(iso)].nominal())) {
      return fail(ErrorKind::Config, "reduction: calculate_f " + std::string(to_string(iso)) +
                                         " is not finite (" + fmt_g(n[index(iso)].nominal()) +
                                         ")");
    }
  }
  const UFloat& n40 = n[index(ArgonIsotope::Ar40)];
  const UFloat& a39 = n[index(ArgonIsotope::Ar39)];
  const UFloat& a38 = n[index(ArgonIsotope::Ar38)];
  const UFloat& a37 = n[index(ArgonIsotope::Ar37)];
  const UFloat& a36 = n[index(ArgonIsotope::Ar36)];

  // :529-531: trapped 40/36, minted once per call, distinct from E12's atm3836.
  const Result<UFloat> trapped = mint(c.atm4036, "trapped_4036");
  if (!trapped) return fail(trapped.error());

  InterferenceOptions o;
  o.mode = c.k3739_mode;
  o.allow_negative_ca_correction = c.allow_negative_ca_correction;
  if (fixed_k3739) {
    const Result<UFloat> fk = mint(*fixed_k3739, "k3739");
    if (!fk) return fail(fk.error());
    o.fixed_k3739 = *fk;
  }
  if (c.k3739_mode == K3739Mode::Fixed) {
    const Result<UFloat> ck = mint(c.fixed_k3739, "k3739");
    if (!ck) return fail(ck.error());
    o.constants_fixed_k3739 = *ck;
  }

  FResult out;
  out.interference = interference_corrections(a39, a37, p, o, &out.diagnostics);
  const InterferenceComponents& ic = out.interference;

  const Result<AtmosphericComponents> atm =
      atmospheric_components(a38, a36, ic.k38, ic.ca38, ic.ca36, decay_days, p.cl3638, c);
  if (!atm) return fail(atm.error());
  out.atmospheric = *atm;

  // :542-545: the cosmogenic split replaces atm36 / atm38.
  if (c.cosmogenic) {
    const Result<CosmogenicComponents> cos =
        cosmogenic_components(out.atmospheric.atm36, out.atmospheric.atm38, *c.cosmogenic);
    if (!cos) return fail(cos.error());
    out.cosmogenic = *cos;
    out.atmospheric.atm36 = cos->noncosmo36;
    out.atmospheric.atm38 = cos->noncosmo38;
  }

  const kernels::FValues<UFloat> fv =
      kernels::f_and_yield(n40, ic.k39, out.atmospheric.atm36, *trapped, p.k4039);
  out.atm40 = fv.atm40;
  out.k40 = fv.k40;
  out.rad40 = fv.rad40;
  if (fv.f_defined) {
    out.f = fv.f;
    const std::array<VariableId, 7> ids = p.interference_ids();
    out.f_err_wo_irrad = std_dev_excluding(fv.f, ids);
  } else {
    out.diagnostics.push_back(Diagnostic::FUndefined);
  }
  if (fv.yield_defined) {
    out.radiogenic_yield = fv.yield;
  } else {
    out.diagnostics.push_back(Diagnostic::YieldUndefined);
  }
  // :582
  out.interference_corrected = {n40 - out.k40, ic.k39, a38, a37, out.atmospheric.atm36};

  // Spec 5.6 / 7: NaN or inf from valid inputs (e.g. an exactly singular E9 or
  // E10 divisor) is flagged once; the values are kept.
  bool all_finite = true;
  const auto check = [&all_finite](const UFloat& x) { all_finite = all_finite && finite(x); };
  if (out.f) {
    check(*out.f);
    all_finite = all_finite && std::isfinite(out.f_err_wo_irrad);
  }
  if (out.radiogenic_yield) check(*out.radiogenic_yield);
  for (const UFloat* x : {&out.atm40, &out.k40, &out.rad40}) check(*x);
  for (const UFloat* x : {&ic.k37, &ic.k38, &ic.k39, &ic.ca36, &ic.ca37, &ic.ca38, &ic.ca39}) {
    check(*x);
  }
  const AtmosphericComponents& am = out.atmospheric;
  for (const UFloat* x : {&am.atm36, &am.atm38, &am.cl36, &am.cl38}) check(*x);
  for (const UFloat& x : out.interference_corrected) check(x);
  if (out.cosmogenic) {
    const CosmogenicComponents& cm = *out.cosmogenic;
    for (const UFloat* x : {&cm.cosmo36, &cm.cosmo38, &cm.noncosmo36, &cm.noncosmo38}) check(*x);
  }
  if (!all_finite) out.diagnostics.push_back(Diagnostic::NonFiniteResult);
  return out;
}

}  // namespace pychron::reduction
