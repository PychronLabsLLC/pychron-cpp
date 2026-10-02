#include "pychron/reduction/arar_types.hpp"

#include <cmath>
#include <string>

namespace pychron::reduction {

Measured resolve(const StoredValue& row) noexcept {
  Measured m{row.value, row.error};
  if (row.use_manual_value) m.value = row.manual_value;
  if (row.use_manual_error) m.error = row.manual_error;
  if (row.modifier_error) m.error = *row.modifier_error;
  return m;
}

std::string_view to_string(ArgonIsotope iso) noexcept {
  switch (iso) {
    case ArgonIsotope::Ar40: return "Ar40";
    case ArgonIsotope::Ar39: return "Ar39";
    case ArgonIsotope::Ar38: return "Ar38";
    case ArgonIsotope::Ar37: return "Ar37";
    case ArgonIsotope::Ar36: return "Ar36";
  }
  return "";
}

IsotopeSignal make_signal(ArgonIsotope iso, const MeasuredSignal& m) {
  const std::string name(to_string(iso));
  IsotopeSignal s;
  s.intercept = UFloat::variable(m.intercept.value, m.intercept.error, name);
  s.baseline = UFloat::variable(m.baseline.value, m.baseline.error, name + " bs");
  s.blank = UFloat::variable(m.blank.value, m.blank.error, name + " bk");
  s.ic_factor = UFloat::variable(m.ic_factor.value, m.ic_factor.error, name + " IC");
  s.include_baseline_error = m.include_baseline_error;
  s.correct_for_blank = m.correct_for_blank;
  s.deadtime_tau_s = m.deadtime_tau_s;
  return s;
}

bool corrects_for_blank(std::string_view t) noexcept {
  for (const std::string_view prefix : {"blank", "detector_ic", "background"}) {
    if (t.starts_with(prefix)) return false;
  }
  return true;
}

ReductionConstants constants_preset(ConstantsPreset p) noexcept {
  ReductionConstants c;
  c.lambda_cl36 = {6.308e-9, 0.0};
  c.lambda_ar37 = {0.01975, 0.0};
  c.lambda_ar39 = {7.068e-6, 0.0};
  c.atm4038 = {1575.0, 2.0};
  c.fixed_k3739 = {0.01, 0.01};
  c.lambda_e = {5.81e-11, 1.6e-13};
  c.lambda_b = {4.962e-10, 9.3e-13};
  switch (p) {
    case ConstantsPreset::Default:
      c.atm4036 = {298.56, 0.31};
      break;
    case ConstantsPreset::Legacy:
      c.atm4036 = {295.5, 0.5};
      c.fixed_k3739.error = 0.0001;
      c.allow_negative_ca_correction = true;
      break;
    case ConstantsPreset::LegacyPreferences:
      c.atm4036 = {295.5, 0.0};
      c.lambda_e.error = 0.0;
      c.lambda_b.error = 0.0;
      break;
  }
  return c;
}

std::string_view to_string(ConstantsPreset p) noexcept {
  switch (p) {
    case ConstantsPreset::Default: return "default";
    case ConstantsPreset::Legacy: return "legacy";
    case ConstantsPreset::LegacyPreferences: return "legacy_preferences";
  }
  return "";
}

Measured lambda_k(const ReductionConstants& c) noexcept {
  const UFloat b = UFloat::variable(c.lambda_b.value, c.lambda_b.error, "lambda_b");
  const UFloat e = UFloat::variable(c.lambda_e.value, c.lambda_e.error, "lambda_e");
  const UFloat k = b + e;
  return {k.nominal(), k.std_dev()};
}

Result<ProductionRatios> production_from_rows(
    const std::map<std::string, Measured, std::less<>>& rows) {
  ProductionRatios out;
  for (const auto& [key, m] : rows) {
    if (!std::isfinite(m.value) || !std::isfinite(m.error)) {
      return fail(ErrorKind::Config, "reduction: production ratio '" + key + "' is not finite");
    }
    if (key == "K4039") out.k4039 = m;
    else if (key == "K3839") out.k3839 = m;
    else if (key == "K3739") out.k3739 = m;
    else if (key == "Ca3937") out.ca3937 = m;
    else if (key == "Ca3837") out.ca3837 = m;
    else if (key == "Ca3637") out.ca3637 = m;
    else if (key == "Cl3638") out.cl3638 = m;
    else if (key == "Ca_K") out.ca_k = m;
    else if (key == "Cl_K") out.cl_k = m;
    else return fail(ErrorKind::Config, "reduction: unknown production ratio key '" + key + "'");
  }
  return out;
}

std::array<VariableId, 7> ProductionVariables::interference_ids() const {
  return {k4039.variable_id(),  k3839.variable_id(),  k3739.variable_id(), ca3937.variable_id(),
          ca3837.variable_id(), ca3637.variable_id(), cl3638.variable_id()};
}

ProductionVariables make_production_variables(const ProductionRatios& p) {
  const auto mk = [](const Measured& m, std::string_view tag) {
    return UFloat::variable(m.value, m.error, tag);
  };
  ProductionVariables v;
  v.k4039 = mk(p.k4039, "K4039");
  v.k3839 = mk(p.k3839, "K3839");
  v.k3739 = mk(p.k3739, "K3739");
  v.ca3937 = mk(p.ca3937, "Ca3937");
  v.ca3837 = mk(p.ca3837, "Ca3837");
  v.ca3637 = mk(p.ca3637, "Ca3637");
  v.cl3638 = mk(p.cl3638, "Cl3638");
  if (p.ca_k) v.ca_k = mk(*p.ca_k, "Ca_K");
  if (p.cl_k) v.cl_k = mk(*p.cl_k, "Cl_K");
  return v;
}

UFloat make_j(const Flux& f) { return UFloat::variable(f.j.value, f.j.error, "J"); }

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

}  // namespace pychron::reduction
