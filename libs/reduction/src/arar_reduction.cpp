// Ar-Ar reduction step functions (spec 6) on the shared kernels (spec 8.1).
#include "pychron/reduction/arar_reduction.hpp"

#include <algorithm>
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

// The one Measured check: a finite value and a finite sigma >= 0, else
// "reduction: <label> must have a finite value and a finite sigma >= 0, got
// v +- e". Step functions pass "constant <name>"; validate() passes the field.
Result<void> validate_constant(const Measured& m, std::string_view label) {
  if (!std::isfinite(m.value) || !std::isfinite(m.error) || m.error < 0.0) {
    return fail(ErrorKind::Config, "reduction: " + std::string(label) +
                                       " must have a finite value and a finite sigma >= 0, got " +
                                       fmt_g(m.value) + " +- " + fmt_g(m.error));
  }
  return {};
}

Result<void> check_constant(const Measured& m, std::string_view name) {
  return validate_constant(m, "constant " + std::string(name));
}

// A constant read by a step function must be a valid variable (finite value,
// finite sigma >= 0) before it is minted.
Result<UFloat> mint(const Measured& m, std::string_view tag) {
  if (auto ok = check_constant(m, tag); !ok) return fail(ok.error());
  return UFloat::variable(m.value, m.error, tag);
}

// atm4036 / atm4038 as `uncertainties` evaluates it for two independent
// variables: nominal a / b, std sqrt((sa / b)^2 + (a sb / b^2)^2), with the
// partials formed exactly as UFloat::operator/ forms them.
// legacy:processing/arar_constants.py:225-226
Result<Measured> atm3836(const ReductionConstants& c) {
  if (auto ok = check_constant(c.atm4036, "atm4036"); !ok) return fail(ok.error());
  if (auto ok = check_constant(c.atm4038, "atm4038"); !ok) return fail(ok.error());
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
  const UFloat* fixed =
      kernels::select_fixed_k3739(o.fixed_k3739, o.mode, o.constants_fixed_k3739);
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

// ---- 3.6 Age ----------------------------------------------------------------

// legacy:processing/arar_constants.py:143-170 (scale_age).
double age_scale(AgeUnits from, AgeUnits to) noexcept {
  double scalar = 1.0;
  switch (from) {
    case AgeUnits::a: scalar = 1.0; break;
    case AgeUnits::ka: scalar = 1e3; break;
    case AgeUnits::Ma: scalar = 1e6; break;
    case AgeUnits::Ga: scalar = 1e9; break;
  }
  double target = 1.0;
  switch (to) {
    case AgeUnits::a: target = 1.0; break;
    case AgeUnits::ka: target = 1e-3; break;
    case AgeUnits::Ma: target = 1e-6; break;
    case AgeUnits::Ga: target = 1e-9; break;
  }
  return scalar * target;
}

namespace {

// Python truthiness of a ufloat (`if lk:`): false only for 0 +- 0.
bool truthy(const Measured& m) noexcept { return !(m.value == 0.0 && m.error == 0.0); }

// E16 lambda_K: the override when truthy (dvc/dvc.py:2303-2305,
// argon_calculations.py:614), else lambda_b + lambda_e (arar_constants.py:263-267).
// Returns its value and sigma; the caller mints the variable.
Result<Measured> resolve_lambda_k(const ReductionConstants& c,
                                  const std::optional<Measured>& lambda_k_total) {
  Measured m;
  if (lambda_k_total && truthy(*lambda_k_total)) {
    if (auto ok = check_constant(*lambda_k_total, "lambda_k_total"); !ok) return fail(ok.error());
    m = *lambda_k_total;
  } else {
    if (auto ok = check_constant(c.lambda_b, "lambda_b"); !ok) return fail(ok.error());
    if (auto ok = check_constant(c.lambda_e, "lambda_e"); !ok) return fail(ok.error());
    m = lambda_k(c);
  }
  if (!std::isfinite(m.value) || !std::isfinite(m.error)) {
    return fail(ErrorKind::Config, "reduction: lambda_K is not finite (" + fmt_g(m.value) +
                                       " +- " + fmt_g(m.error) + ")");
  }
  if (m.value == 0.0) {
    return fail(ErrorKind::Config,
                "reduction: lambda_K (lambda_k_total or lambda_b + lambda_e) is zero");
  }
  return m;
}

// A fresh lambda_K variable, or nullopt when the decay constant enters as its
// nominal only (include_decay_error false, argon_calculations.py:622-623).
std::optional<UFloat> lambda_variable(const ReductionConstants& c, const Measured& m) {
  if (!c.include_decay_error) return std::nullopt;
  return UFloat::variable(m.value, m.error, "lambda_k");
}

// E16 on resolved inputs; `lambda` null means nominal lambda_K.
kernels::Age<UFloat> age_of(const UFloat& j, const UFloat& f, const Measured& lk,
                            const std::optional<UFloat>& lambda, AgeUnits units) {
  const double scale = age_scale(AgeUnits::a, units);
  if (lambda) return kernels::age(j, f, *lambda, scale);
  return kernels::age(j, f, lk.value, scale);
}

Result<void> check_j_f(const UFloat& j, const UFloat& f) {
  if (!std::isfinite(j.nominal()) || !std::isfinite(f.nominal())) {
    return fail(ErrorKind::Config, "reduction: age J and F must be finite (J = " +
                                       fmt_g(j.nominal()) + ", F = " + fmt_g(f.nominal()) + ")");
  }
  return {};
}

}  // namespace

// E16. legacy:processing/argon_calculations.py:603-630 (age_equation).
Result<UFloat> age_equation(const UFloat& j, const UFloat& f, const ReductionConstants& c,
                            std::optional<Measured> lambda_k_total) {
  if (auto ok = check_j_f(j, f); !ok) return fail(ok.error());
  const Result<Measured> lk = resolve_lambda_k(c, lambda_k_total);
  if (!lk) return fail(lk.error());
  const kernels::Age<UFloat> a = age_of(j, f, *lk, lambda_variable(c, *lk), c.age_units);
  if (!a.defined) {
    return fail(ErrorKind::Config, "reduction: age undefined, 1 + J F <= 0 (J = " +
                                       fmt_g(j.nominal()) + ", F = " + fmt_g(f.nominal()) + ")");
  }
  return a.age;
}

namespace detail {

// E17-E18. legacy:processing/arar_age.py:658-686 (_set_age_values).
Result<std::optional<AgeSet>> make_age_set(const UFloat& j, double position_jerr,
                                           const UFloat& f, const ReductionConstants& c,
                                           std::optional<Measured> lambda_k_total,
                                           std::span<const VariableId> interference_ids) {
  if (!std::isfinite(position_jerr) || position_jerr < 0.0) {
    return fail(ErrorKind::Config, "reduction: position_jerr must be finite and >= 0, got " +
                                       fmt_g(position_jerr));
  }
  if (auto ok = check_j_f(j, f); !ok) return fail(ok.error());
  const Result<Measured> lk = resolve_lambda_k(c, lambda_k_total);
  if (!lk) return fail(lk.error());
  // An override is one variable on the constants, shared by the three
  // variants; otherwise each legacy age_equation call reads lambda_K afresh.
  const bool shared = lambda_k_total && truthy(*lambda_k_total);
  const std::optional<UFloat> shared_lambda =
      shared ? lambda_variable(c, *lk) : std::optional<UFloat>{};
  const auto lambda = [&]() { return shared ? shared_lambda : lambda_variable(c, *lk); };

  const double j_nominal = j.nominal();
  // :678 J'' = ufloat(nom(J), position_jerr, tag="Position")
  const UFloat j_position = UFloat::variable(j_nominal, position_jerr, "Position");
  // :689 J' = ufloat(nom(J), 0, tag="J_no_err"): exact, so no term.
  const UFloat j_no_err = UFloat::variable(j_nominal, 0.0, "J_no_err");

  const kernels::Age<UFloat> pos = age_of(j_position, f, *lk, lambda(), c.age_units);
  const kernels::Age<UFloat> w_j = age_of(j, f, *lk, lambda(), c.age_units);
  const kernels::Age<UFloat> plain = age_of(j_no_err, f, *lk, lambda(), c.age_units);
  // Same nominal 1 + J F in all three, so all are defined or none is.
  if (!plain.defined || !w_j.defined || !pos.defined) return std::optional<AgeSet>{};

  AgeSet out;
  out.age = plain.age;
  out.age_w_j_err = w_j.age;
  out.age_w_position_err = pos.age;
  // E18 (spec Q13): legacy declares these but never assigns them.
  out.age_err_wo_irrad = std_dev_excluding(out.age, interference_ids);
  out.age_err_wo_j_irrad = out.age_err_wo_irrad;
  return std::optional<AgeSet>{std::move(out)};
}

}  // namespace detail

// ---- Whole pipeline ---------------------------------------------------------

namespace {

Result<void> invalid(std::string what) {
  return fail(ErrorKind::Config, "reduction: " + std::move(what));
}

// A UFloat input: finite nominal, every variable's sigma finite and >= 0, and
// a finite standard deviation.
Result<void> check_ufloat(const UFloat& x, const std::string& name) {
  if (!std::isfinite(x.nominal())) {
    return invalid(name + " is not finite (" + fmt_g(x.nominal()) + ")");
  }
  for (const UFloat::Term& t : x.terms()) {
    if (!std::isfinite(t.sigma) || t.sigma < 0.0 || !std::isfinite(t.deriv)) {
      return invalid(name + " has a non-finite or negative sigma (" + fmt_g(t.sigma) + ")");
    }
  }
  if (!std::isfinite(x.std_dev())) {
    return invalid(name + " has a non-finite standard deviation");
  }
  return {};
}

// Spec 6 input policy: every field reduce() reads, before any arithmetic.
Result<void> validate(const ReductionInput& in) {
  for (const ArgonIsotope iso : kArgonKeys) {
    const IsotopeSignal& s = in.isotopes[index(iso)];
    const std::string n(to_string(iso));
    // Q7: a non-finite intercept is an error naming the slot (legacy coerced
    // it to 0, isotope.py:465-468).
    for (const auto& [x, field] : {std::pair{&s.intercept, "intercept"},
                                   std::pair{&s.baseline, "baseline"},
                                   std::pair{&s.blank, "blank"},
                                   std::pair{&s.ic_factor, "ic_factor"},
                                   std::pair{&s.discrimination, "discrimination"}}) {
      if (auto ok = check_ufloat(*x, n + " " + field); !ok) return ok;
    }
    if (s.deadtime_tau_s && (!std::isfinite(*s.deadtime_tau_s) || *s.deadtime_tau_s < 0.0)) {
      return invalid(n + " deadtime_tau_s must be finite and >= 0, got " +
                     fmt_g(*s.deadtime_tau_s));
    }
  }

  const ReductionConstants& c = in.constants;
  // lambda_K first, so a value-initialised record (D1) names it.
  const bool lk_override =
      in.lambda_k_total && !(in.lambda_k_total->value == 0.0 && in.lambda_k_total->error == 0.0);
  if (in.lambda_k_total) {
    if (auto ok = validate_constant(*in.lambda_k_total, "lambda_k_total"); !ok) return ok;
  }
  if (!lk_override && c.lambda_b.value + c.lambda_e.value == 0.0) {
    return invalid("constants lambda_b + lambda_e is zero (lambda_K must be nonzero)");
  }
  for (const auto& [m, field] :
       {std::pair{&c.lambda_b, "lambda_b"}, std::pair{&c.lambda_e, "lambda_e"},
        std::pair{&c.lambda_cl36, "lambda_cl36"}, std::pair{&c.lambda_ar37, "lambda_ar37"},
        std::pair{&c.lambda_ar39, "lambda_ar39"}, std::pair{&c.atm4036, "atm4036"},
        std::pair{&c.atm4038, "atm4038"}, std::pair{&c.fixed_k3739, "fixed_k3739"}}) {
    if (auto ok = validate_constant(*m, std::string("constants ") + field); !ok) return ok;
  }
  if (c.cosmogenic) {
    if (auto ok = validate_constant(c.cosmogenic->solar3836, "constants solar3836"); !ok) return ok;
    if (auto ok = validate_constant(c.cosmogenic->cosmo3836, "constants cosmo3836"); !ok) return ok;
  }
  if (!std::isfinite(c.abundance_sensitivity) || c.abundance_sensitivity < 0.0) {
    return invalid("constants abundance_sensitivity must be finite and >= 0, got " +
                   fmt_g(c.abundance_sensitivity));
  }

  const ProductionVariables& p = in.production;
  for (const auto& [x, field] :
       {std::pair{&p.k4039, "K4039"}, std::pair{&p.k3839, "K3839"}, std::pair{&p.k3739, "K3739"},
        std::pair{&p.ca3937, "Ca3937"}, std::pair{&p.ca3837, "Ca3837"},
        std::pair{&p.ca3637, "Ca3637"}, std::pair{&p.cl3638, "Cl3638"}}) {
    if (auto ok = check_ufloat(*x, std::string("production ") + field); !ok) return ok;
  }
  if (p.ca_k) {
    if (auto ok = check_ufloat(*p.ca_k, "production Ca_K"); !ok) return ok;
  }
  if (p.cl_k) {
    if (auto ok = check_ufloat(*p.cl_k, "production Cl_K"); !ok) return ok;
  }

  if (!std::isfinite(in.irradiation.decay_days)) {
    return invalid("irradiation decay_days is not finite (" +
                   fmt_g(in.irradiation.decay_days) + ")");
  }
  for (const DecaySegment& s : in.irradiation.segments) {
    if (!std::isfinite(s.power) || !std::isfinite(s.duration_days) ||
        !std::isfinite(s.dt_days)) {
      return invalid("irradiation segment is not finite (power " + fmt_g(s.power) +
                     ", duration_days " + fmt_g(s.duration_days) + ", dt_days " +
                     fmt_g(s.dt_days) + ")");
    }
  }

  if (in.j) {
    if (auto ok = check_ufloat(*in.j, "J"); !ok) return ok;
  }
  if (!std::isfinite(in.position_jerr) || in.position_jerr < 0.0) {
    return invalid("position_jerr must be finite and >= 0, got " + fmt_g(in.position_jerr));
  }
  if (in.fixed_k3739) {
    if (auto ok = validate_constant(*in.fixed_k3739, "fixed_k3739"); !ok) return ok;
  }
  return {};
}

std::string without_prefix(std::string_view what) {
  constexpr std::string_view prefix = "reduction: ";
  if (what.starts_with(prefix)) what.remove_prefix(prefix.size());
  return std::string(what);
}

// The E20 tags, interned once.
const std::array<TagId, 5>& isotope_tags() {
  static const std::array<TagId, 5> tags{intern_tag("Ar40"), intern_tag("Ar39"),
                                         intern_tag("Ar38"), intern_tag("Ar37"),
                                         intern_tag("Ar36")};
  return tags;
}

}  // namespace

// legacy:processing/arar_age.py:443-689 (calculate_age, _assemble_isotope_intensities,
// _calculate_f, _set_age_values, _calculate_kca, _calculate_kcl, get_error_component).
Result<ArArResult> reduce(const ReductionInput& in) {
  if (auto ok = validate(in); !ok) return fail(ok.error());
  const ReductionConstants& c = in.constants;
  ArArResult out;

  // E5 (D4) on the intercept, then E1-E3 (isotope.py:820-854).
  std::array<UFloat, 5> s;
  for (const ArgonIsotope iso : kArgonKeys) {
    const IsotopeSignal& sig = in.isotopes[index(iso)];
    if (!sig.deadtime_tau_s) {
      s[index(iso)] = corrected_intensity(sig);
      continue;
    }
    const Result<UFloat> intercept = deadtime_corrected_intercept(sig);
    if (!intercept) {
      return fail(ErrorKind::Config, "reduction: " + std::string(to_string(iso)) + " " +
                                         without_prefix(intercept.error().what));
    }
    IsotopeSignal corrected = sig;
    corrected.intercept = *intercept;
    s[index(iso)] = corrected_intensity(corrected);
  }

  // E4, applied unconditionally as legacy does (arar_age.py:589-591).
  s = abundance_sensitivity_correction(s, c.abundance_sensitivity);

  // E7 on nominal per-day lambdas (arar_age.py:455-463), E8 on 37 and 39 by
  // name (arar_age.py:596-599).
  const Result<DecayFactors> df =
      decay_factors(c.lambda_ar37.value, c.lambda_ar39.value, in.irradiation.segments);
  if (!df) return fail(df.error());
  out.decay = *df;
  s[index(ArgonIsotope::Ar39)] = s[index(ArgonIsotope::Ar39)] * df->df39;
  s[index(ArgonIsotope::Ar37)] = s[index(ArgonIsotope::Ar37)] * df->df37;
  out.corrected = s;

  // E9-E15.
  Result<FResult> f = calculate_f(s, in.irradiation.decay_days, in.production, c, in.fixed_k3739);
  if (!f) return fail(f.error());
  out.f = std::move(*f);
  out.diagnostics = out.f.diagnostics;

  // E16-E18 (arar_age.py:658-689): only with J and a finite F. A non-finite F
  // (NonFiniteResult already raised) gives no ages rather than an error.
  if (in.j && out.f.f && std::isfinite(out.f.f->nominal())) {
    const std::array<VariableId, 7> ids = in.production.interference_ids();
    Result<std::optional<AgeSet>> ages = detail::make_age_set(
        *in.j, in.position_jerr, *out.f.f, c, in.lambda_k_total, ids);
    if (!ages) return fail(ages.error());
    if (*ages) {
      out.ages = std::move(**ages);
    } else {
      out.diagnostics.push_back(Diagnostic::AgeUndefined);  // spec Q6, D3
    }
  }

  // E19 K/Ca (arar_age.py:534-545, :560-566): k39 / ca37 * (1 / Ca_K), the
  // factor 1 when Ca_K is missing or nominally 0. ca37 is the E11-clamped
  // value. ca37 == 0 (legacy ZeroDivisionError -> kca = 0) leaves both absent;
  // so does kca == 0 for cak, keeping kca.
  const UFloat& k39 = out.f.interference.k39;
  const UFloat& ca37 = out.f.interference.ca37;
  if (ca37.nominal() == 0.0) {
    out.diagnostics.push_back(Diagnostic::KCaUndefined);
  } else {
    UFloat kca = k39 / ca37;
    if (in.production.ca_k && in.production.ca_k->nominal() != 0.0) {
      kca = kca * (1.0 / *in.production.ca_k);
    }
    if (kca.nominal() == 0.0) {
      out.diagnostics.push_back(Diagnostic::KCaUndefined);
    } else {
      out.cak = 1.0 / kca;
    }
    out.kca = std::move(kca);
  }

  // E19 K/Cl (arar_age.py:547-558, :560-566): k39 / cl38 * (1 / Cl_K), the
  // factor 1 when Cl_K is missing or nominally 0. cl38 is E12's residual 38
  // (also without Cl production, legacy behaviour). cl38 == 0 (legacy
  // ZeroDivisionError -> kcl = 0) leaves both absent; kcl == 0 keeps kcl and
  // leaves clk absent, as for K/Ca.
  const UFloat& cl38 = out.f.atmospheric.cl38;
  if (cl38.nominal() == 0.0) {
    out.diagnostics.push_back(Diagnostic::KClUndefined);
  } else {
    UFloat kcl = k39 / cl38;
    if (in.production.cl_k && in.production.cl_k->nominal() != 0.0) {
      kcl = kcl * (1.0 / *in.production.cl_k);
    }
    if (kcl.nominal() == 0.0) {
      out.diagnostics.push_back(Diagnostic::KClUndefined);
    } else {
      out.clk = 1.0 / kcl;
    }
    out.kcl = std::move(kcl);
  }

  // E20 on age_w_j_err by isotope tag (arar_age.py:214-229).
  if (out.ages) {
    const std::array<TagId, 5>& tags = isotope_tags();
    for (const ArgonIsotope iso : kArgonKeys) {
      out.age_error_components.emplace(std::string(to_string(iso)),
                                       variance_percent(out.ages->age_w_j_err, tags[index(iso)]));
    }
  }

  // Spec 5.6 / 7: NaN or inf from valid inputs, flagged once, values kept.
  if (std::find(out.diagnostics.begin(), out.diagnostics.end(), Diagnostic::NonFiniteResult) ==
      out.diagnostics.end()) {
    bool all_finite = true;
    const auto check = [&all_finite](const UFloat& x) { all_finite = all_finite && finite(x); };
    for (const UFloat& x : out.corrected) check(x);
    if (out.ages) {
      check(out.ages->age);
      check(out.ages->age_w_j_err);
      check(out.ages->age_w_position_err);
      all_finite = all_finite && std::isfinite(out.ages->age_err_wo_irrad);
    }
    if (out.kca) check(*out.kca);
    if (out.cak) check(*out.cak);
    if (out.kcl) check(*out.kcl);
    if (out.clk) check(*out.clk);
    for (const auto& [k, v] : out.age_error_components) all_finite = all_finite && std::isfinite(v);
    if (!all_finite) out.diagnostics.push_back(Diagnostic::NonFiniteResult);
  }
  return out;
}

}  // namespace pychron::reduction
