// F, radiogenic yield and the irradiation-free F error (spec 3.5, E9-E15;
// Q6/D3; Review Focus 1, 3, 4).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "golden.hpp"
#include "pychron/reduction/arar_reduction.hpp"

using namespace pychron::reduction;
namespace g = pychron::reduction::golden;

namespace {

std::string fmt(const char* what, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%g", v);
  return std::string(what) + buf;
}

std::vector<std::string> names_of(const std::vector<Diagnostic>& d) {
  std::vector<std::string> out;
  for (const Diagnostic x : d) out.emplace_back(to_string(x));
  return out;
}

bool has(const std::vector<Diagnostic>& d, Diagnostic x) {
  return std::find(d.begin(), d.end(), x) != d.end();
}

std::vector<VariableId> ids_tagged(const UFloat& x, std::string_view tag) {
  std::vector<VariableId> out;
  for (const UFloat::Term& t : x.terms()) {
    if (tag_name(t.tag) == tag) out.push_back(t.id);
  }
  return out;
}

// Same nominal and the same derivative for every variable of either side.
void expect_identical(const UFloat& got, const UFloat& want, const std::string& what) {
  EXPECT_EQ(got.nominal(), want.nominal()) << what;
  for (const UFloat::Term& t : want.terms()) EXPECT_EQ(got.derivative(t.id), t.deriv) << what;
  for (const UFloat::Term& t : got.terms()) EXPECT_EQ(want.derivative(t.id), t.deriv) << what;
}

void expect_rel(double got, double want, double rtol, const std::string& what) {
  EXPECT_LE(std::fabs(got - want), rtol * std::fabs(want))
      << what << ": got " << ::testing::PrintToString(got) << ", want "
      << ::testing::PrintToString(want);
}

// legacy argon_calculations_test.py _isotopes(): Ar40, Ar39, Ar38, Ar37, Ar36.
std::array<UFloat, 5> legacy_isotopes() {
  return {UFloat::variable(1000.0, 1.0, "Ar40"), UFloat::variable(100.0, 0.5, "Ar39"),
          UFloat::variable(10.0, 0.05, "Ar38"), UFloat::variable(1.0, 0.005, "Ar37"),
          UFloat::variable(2.0, 0.01, "Ar36")};
}

// legacy_isotopes() with Ar37 = 5 (golden young_volcanic), so that ca37 and
// the Ca-derived corrections are non-zero (with Ar37 = 1, ca37 is ~0).
std::array<UFloat, 5> isotopes_with_ca() {
  std::array<UFloat, 5> n = legacy_isotopes();
  n[3] = UFloat::variable(5.0, 0.025, "Ar37");
  return n;
}

// legacy _interferences() (all exact).
ProductionRatios legacy_interferences() {
  ProductionRatios p;
  p.ca3937 = {0.0007, 0};
  p.k3739 = {0.01, 0};
  p.k3839 = {0.013, 0};
  p.ca3637 = {0.00026, 0};
  p.ca3837 = {0.00019, 0};
  p.k4039 = {0.0002, 0};
  p.cl3638 = {0.0, 0};
  return p;
}

// The same ratios, each with a 1 % error.
ProductionRatios ratios_with_errors() {
  ProductionRatios p = legacy_interferences();
  for (Measured* m : {&p.ca3937, &p.k3739, &p.k3839, &p.ca3637, &p.ca3837, &p.k4039}) {
    m->error = 0.01 * m->value;
  }
  p.cl3638 = {250.0, 5.0};
  return p;
}

ReductionConstants legacy_constants() {
  ReductionConstants c = constants_preset(ConstantsPreset::Legacy);
  c.lambda_cl36 = {6.308e-9, 1e-11};
  return c;
}

const std::array<std::string_view, 7> kInterferenceTags{"K4039",  "K3839",  "K3739", "Ca3937",
                                                        "Ca3837", "Ca3637", "Cl3638"};

// Independent recomputation: std over terms whose tag is not an interference
// ratio's.
double std_without_interference_tags(const UFloat& x) {
  double v = 0.0;
  for (const UFloat::Term& t : x.terms()) {
    if (std::find(kInterferenceTags.begin(), kInterferenceTags.end(), tag_name(t.tag)) !=
        kInterferenceTags.end()) {
      continue;
    }
    const double c = t.deriv * t.sigma;
    v += c * c;
  }
  return std::sqrt(v);
}

Measured measured_of(const g::Json& j) { return {j["v"].as_number(), j["e"].as_number()}; }

}  // namespace

// legacy CalculateFTest test_f_equals_rad40_over_k39, test_f_wo_irrad_has_smaller_error,
// test_radiogenic_yield_is_percent
TEST(CalculateF, EqualsRad40OverK39) {
  const std::array<UFloat, 5> n = legacy_isotopes();
  const ProductionVariables p = make_production_variables(legacy_interferences());
  const auto r = calculate_f(n, 365.0, p, legacy_constants());
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_TRUE(r->f.has_value());
  EXPECT_NEAR(r->f->nominal(), r->rad40.nominal() / r->interference.k39.nominal(), 1e-10);
  expect_identical(*r->f, r->rad40 / r->interference.k39, "f");
  EXPECT_LE(r->f_err_wo_irrad, r->f->std_dev());
  ASSERT_TRUE(r->radiogenic_yield.has_value());
  EXPECT_GE(r->radiogenic_yield->nominal(), 0.0);
  EXPECT_LE(r->radiogenic_yield->nominal(), 100.0);
  expect_identical(*r->radiogenic_yield, r->rad40 / n[0] * 100.0, "yield");
  EXPECT_TRUE(r->diagnostics.empty()) << ::testing::PrintToString(names_of(r->diagnostics));
  EXPECT_FALSE(r->cosmogenic.has_value());
}

// legacy test_interference_corrected_keys, test_non_ar_isotope_keys and the
// `ifc` map (argon_calculations.py:582).
TEST(CalculateF, InterferenceCorrectedMap) {
  const std::array<UFloat, 5> n = isotopes_with_ca();
  const ProductionVariables p = make_production_variables(ratios_with_errors());
  const auto r = calculate_f(n, 365.0, p, legacy_constants());
  ASSERT_TRUE(r) << r.error().what;
  const auto& ifc = r->interference_corrected;
  expect_identical(ifc[index(ArgonIsotope::Ar40)], n[0] - r->k40, "Ar40");
  expect_identical(ifc[index(ArgonIsotope::Ar39)], r->interference.k39, "Ar39");
  expect_identical(ifc[index(ArgonIsotope::Ar38)], n[2], "Ar38");
  expect_identical(ifc[index(ArgonIsotope::Ar37)], n[3], "Ar37");
  expect_identical(ifc[index(ArgonIsotope::Ar36)], r->atmospheric.atm36, "Ar36");
  // E14 pieces.
  expect_identical(r->k40, r->interference.k39 * p.k4039, "k40");
  expect_identical(r->rad40, n[0] - r->atm40 - r->k40, "rad40");
  // The chlorine branch is live with Cl3638 > 0.
  EXPECT_NE(r->atmospheric.cl36.nominal(), 0.0);
  EXPECT_NE(r->interference.ca37.nominal(), 0.0);
}

// Review Focus 4 / E15: one pass, the error with exactly the seven interference
// ratio variables treated as exact.
TEST(CalculateF, WithoutIrradExcludesExactlyInterferenceIds) {
  ProductionRatios pr = ratios_with_errors();
  const ProductionVariables p = make_production_variables(pr);
  const ReductionConstants c = legacy_constants();
  const auto r = calculate_f(isotopes_with_ca(), 365.0, p, c);
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_TRUE(r->f.has_value());
  const auto ids = p.interference_ids();
  EXPECT_EQ(r->f_err_wo_irrad, std_dev_excluding(*r->f, ids));
  expect_rel(r->f_err_wo_irrad, std_without_interference_tags(*r->f), 1e-14, "by tag");
  EXPECT_LT(r->f_err_wo_irrad, r->f->std_dev());
  for (const VariableId id : ids) EXPECT_NE(r->f->derivative(id), 0.0) << id;

  // Ca_K / Cl_K do not enter F or its irradiation-free error.
  pr.ca_k = Measured{0.5, 0.05};
  pr.cl_k = Measured{0.2, 0.02};
  const ProductionVariables pk = make_production_variables(pr);
  const auto rk = calculate_f(isotopes_with_ca(), 365.0, pk, c);
  ASSERT_TRUE(rk) << rk.error().what;
  ASSERT_TRUE(rk->f.has_value());
  EXPECT_EQ(rk->f->derivative(pk.ca_k->variable_id()), 0.0);
  EXPECT_EQ(rk->f->derivative(pk.cl_k->variable_id()), 0.0);
  expect_rel(rk->f_err_wo_irrad, r->f_err_wo_irrad, 1e-14, "with Ca_K/Cl_K");

  // Legacy second pass (argon_calculations.py:585-589): every ratio re-minted
  // with zero error. Equal by linearity (spec E15).
  ProductionRatios zero = pr;
  for (Measured* m : {&zero.k4039, &zero.k3839, &zero.k3739, &zero.ca3937, &zero.ca3837,
                      &zero.ca3637, &zero.cl3638}) {
    m->error = 0.0;
  }
  const auto r0 = calculate_f(isotopes_with_ca(), 365.0, make_production_variables(zero), c);
  ASSERT_TRUE(r0 && r0->f);
  expect_rel(r->f_err_wo_irrad, r0->f->std_dev(), 1e-12, "legacy two-pass");
}

// Controller ruling: some ratios exact (variable id 0 in interference_ids());
// id 0 must exclude nothing and must not drop other contributions.
TEST(CalculateF, WithoutIrradWithZeroErrorRatios) {
  ProductionRatios pr = ratios_with_errors();
  pr.k3739.error = 0.0;
  pr.k4039.error = 0.0;
  pr.cl3638.error = 0.0;
  const ProductionVariables p = make_production_variables(pr);
  const auto ids = p.interference_ids();
  ASSERT_EQ(std::count(ids.begin(), ids.end(), VariableId{0}), 3);
  const ReductionConstants c = legacy_constants();
  const auto r = calculate_f(isotopes_with_ca(), 365.0, p, c);
  ASSERT_TRUE(r && r->f);
  expect_rel(r->f_err_wo_irrad, std_without_interference_tags(*r->f), 1e-14, "by tag");
  EXPECT_GT(r->f_err_wo_irrad, 0.0);
  EXPECT_LT(r->f_err_wo_irrad, r->f->std_dev());

  ProductionRatios zero = pr;
  for (Measured* m : {&zero.k3839, &zero.ca3937, &zero.ca3837, &zero.ca3637}) m->error = 0.0;
  const auto r0 = calculate_f(isotopes_with_ca(), 365.0, make_production_variables(zero), c);
  ASSERT_TRUE(r0 && r0->f);
  expect_rel(r->f_err_wo_irrad, r0->f->std_dev(), 1e-12, "legacy two-pass");

  // All ratios exact: nothing is excluded, f_err_wo_irrad == std(F).
  const ProductionVariables pe = make_production_variables(legacy_interferences());
  const auto re = calculate_f(isotopes_with_ca(), 365.0, pe, c);
  ASSERT_TRUE(re && re->f);
  EXPECT_EQ(re->f_err_wo_irrad, re->f->std_dev());
}

// Review Focus 1 / spec Q1: E14's trapped 40/36 and E12's 38/36 ratio are
// distinct fresh variables.
TEST(CalculateF, TrappedAndRatioAtmAreDistinct) {
  ProductionRatios pr = legacy_interferences();
  pr.cl3638 = {250.0, 5.0};
  const ProductionVariables p = make_production_variables(pr);
  const auto r = calculate_f(legacy_isotopes(), 365.0, p, legacy_constants());
  ASSERT_TRUE(r && r->f);
  const auto trapped = ids_tagged(*r->f, "trapped_4036");
  const auto ratio = ids_tagged(*r->f, "atm3836");
  ASSERT_EQ(trapped.size(), 1u);
  ASSERT_EQ(ratio.size(), 1u);
  EXPECT_NE(trapped[0], ratio[0]);
  EXPECT_TRUE(ids_tagged(*r->f, "atm4036").empty());
  EXPECT_TRUE(ids_tagged(*r->f, "atm4038").empty());
  // trapped_4036 enters only through atm40 = atm36 T; atm36 does not carry it.
  EXPECT_TRUE(ids_tagged(r->atmospheric.atm36, "trapped_4036").empty());
  EXPECT_EQ(ids_tagged(r->atm40, "trapped_4036"), trapped);
  EXPECT_EQ(r->atm40.derivative(trapped[0]), r->atmospheric.atm36.nominal());
  // A second call mints a new trapped_4036.
  const auto r2 = calculate_f(legacy_isotopes(), 365.0, p, legacy_constants());
  ASSERT_TRUE(r2 && r2->f);
  EXPECT_NE(ids_tagged(*r2->f, "trapped_4036"), trapped);
}

// Spec Q6 / D3 / Review Focus 3: no F = 1 +- 0 sentinel.
TEST(CalculateF, K39ZeroIsUndefined) {
  std::array<UFloat, 5> n = legacy_isotopes();
  n[1] = UFloat::variable(0.0, 0.5, "Ar39");
  n[3] = UFloat::variable(0.0, 0.005, "Ar37");
  const ProductionVariables p = make_production_variables(legacy_interferences());
  const auto r = calculate_f(n, 365.0, p, legacy_constants());
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->interference.k39.nominal(), 0.0);
  EXPECT_FALSE(r->f.has_value());
  EXPECT_EQ(r->f_err_wo_irrad, 0.0);
  EXPECT_TRUE(has(r->diagnostics, Diagnostic::FUndefined));
  EXPECT_FALSE(has(r->diagnostics, Diagnostic::NonFiniteResult));
  EXPECT_EQ(r->rad40.nominal(), 409.0);
  EXPECT_GT(r->rad40.std_dev(), 0.0);
  EXPECT_TRUE(r->radiogenic_yield.has_value());

  // Near zero (non-zero) divides: no epsilon threshold (spec 7).
  ReductionConstants c = constants_preset(ConstantsPreset::LegacyPreferences);
  c.allow_negative_ca_correction = true;
  std::array<UFloat, 5> tiny{UFloat::variable(1000.0, 1e-200, "Ar40"), UFloat(1e-300),
                             UFloat(10.0), UFloat(0.0), UFloat(2.0)};
  const auto rt = calculate_f(tiny, 365.0, p, c);
  ASSERT_TRUE(rt) << rt.error().what;
  ASSERT_TRUE(rt->f.has_value());
  EXPECT_GT(rt->f->nominal(), 1e300);
  EXPECT_TRUE(std::isfinite(rt->f->nominal()));
  EXPECT_TRUE(std::isfinite(rt->f->std_dev()));
  EXPECT_TRUE(rt->diagnostics.empty()) << ::testing::PrintToString(names_of(rt->diagnostics));
}

TEST(CalculateF, A40ZeroYieldUndefined) {
  std::array<UFloat, 5> n = legacy_isotopes();
  n[0] = UFloat::variable(0.0, 1.0, "Ar40");
  const ProductionVariables p = make_production_variables(legacy_interferences());
  const auto r = calculate_f(n, 365.0, p, legacy_constants());
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_FALSE(r->radiogenic_yield.has_value());
  EXPECT_EQ(names_of(r->diagnostics), std::vector<std::string>{"YieldUndefined"});
  ASSERT_TRUE(r->f.has_value());
  EXPECT_LT(r->f->nominal(), 0.0);
}

// E13 when enabled replaces atm36/atm38 before E14 (argon_calculations.py:542-545).
TEST(CalculateF, CosmogenicReplacesAtm) {
  ReductionConstants c = legacy_constants();
  c.cosmogenic = CosmogenicRatios{{0.18, 0.001}, {0.65, 0.01}};
  const ProductionVariables p = make_production_variables(legacy_interferences());
  const auto r = calculate_f(legacy_isotopes(), 365.0, p, c);
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_TRUE(r->cosmogenic.has_value());
  const auto& ifc = r->interference_corrected;
  expect_identical(ifc[index(ArgonIsotope::Ar36)], r->cosmogenic->noncosmo36, "Ar36");
  expect_identical(r->atmospheric.atm36, r->cosmogenic->noncosmo36, "atm36");
  expect_identical(r->atmospheric.atm38, r->cosmogenic->noncosmo38, "atm38");
  const auto t = ids_tagged(r->atm40, "trapped_4036");
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(r->atm40.derivative(t[0]), r->cosmogenic->noncosmo36.nominal());
  EXPECT_FALSE(ids_tagged(*r->f, "cosmo3836").empty());
}

// Spec 5.6 / 7: NaN or inf from valid inputs is flagged, value kept.
TEST(CalculateF, SingularNormalModeIsNonFinite) {
  ProductionRatios pr = legacy_interferences();
  pr.k3739 = {2.0, 0.0};
  pr.ca3937 = {0.5, 0.0};  // 1 - K3739 Ca3937 == 0 (E9)
  const auto r = calculate_f(legacy_isotopes(), 365.0, make_production_variables(pr),
                             legacy_constants());
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_FALSE(std::isfinite(r->interference.k39.nominal()));
  ASSERT_TRUE(r->f.has_value());
  EXPECT_FALSE(std::isfinite(r->f->nominal()));
  EXPECT_EQ(std::count(r->diagnostics.begin(), r->diagnostics.end(),
                       Diagnostic::NonFiniteResult),
            1);
}

TEST(CalculateF, SingularFixedModeIsNonFinite) {
  ProductionRatios pr = legacy_interferences();
  pr.ca3937 = {-0.5, 0.0};  // y = -2, x + y == 0 (E10)
  const auto r = calculate_f(legacy_isotopes(), 365.0, make_production_variables(pr),
                             legacy_constants(), Measured{2.0, 0.0});
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_FALSE(std::isfinite(r->interference.ca37.nominal()));
  EXPECT_TRUE(has(r->diagnostics, Diagnostic::NonFiniteResult));
}

TEST(CalculateF, InvalidInputsError) {
  const ProductionVariables p = make_production_variables(legacy_interferences());
  ReductionConstants c = legacy_constants();
  c.atm4036.error = -1.0;
  const auto r = calculate_f(legacy_isotopes(), 365.0, p, c);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
  EXPECT_TRUE(r.error().what.starts_with("reduction: ")) << r.error().what;

  std::array<UFloat, 5> n = legacy_isotopes();
  n[4] = UFloat(std::nan(""));
  const auto rn = calculate_f(n, 365.0, p, legacy_constants());
  ASSERT_FALSE(rn);
  EXPECT_NE(rn.error().what.find("Ar36"), std::string::npos) << rn.error().what;

  const auto rk = calculate_f(legacy_isotopes(), 365.0, p, legacy_constants(),
                              Measured{0.01, -1.0});
  ASSERT_FALSE(rk);
  EXPECT_TRUE(rk.error().what.starts_with("reduction: ")) << rk.error().what;
}

TEST(CalculateF, Golden) {
  const g::Json doc = g::load("calculate_f.json");
  const g::Json& cases = doc["cases"];
  ASSERT_EQ(cases.size(), 24u);
  std::size_t n_legacy = 0, n_prefs = 0, n_sentinel = 0, n_cosmo = 0, n_diag = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const g::Json& c = cases[i];
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    const g::Json& in = c["inputs"];
    if (in["function"].as_string() != "calculate_f") {
      ADD_FAILURE() << name << ": unhandled function";
      continue;
    }
    if (!c["expect_error"].is_null()) ADD_FAILURE() << name << ": unhandled expect_error";
    if (name.ends_with("@legacy")) ++n_legacy;
    else if (name.ends_with("@legacy_preferences")) ++n_prefs;
    const g::Tol t = g::tol_of(c);

    // Constants: every key is read or explicitly irrelevant to calculate_f.
    const g::Json& k = in["constants"];
    ReductionConstants rc;
    bool ok = true;
    for (const auto& [key, v] : k.as_object()) {
      if (key == "lambda_cl36") rc.lambda_cl36 = measured_of(v);
      else if (key == "atm4036") rc.atm4036 = measured_of(v);
      else if (key == "atm4038") rc.atm4038 = measured_of(v);
      else if (key == "fixed_k3739") rc.fixed_k3739 = measured_of(v);
      else if (key == "allow_negative_ca_correction") rc.allow_negative_ca_correction = v.as_bool();
      else if (key == "k3739_mode") {
        if (v.as_string() == "Fixed") {
          rc.k3739_mode = K3739Mode::Fixed;
        } else if (v.as_string() != "Normal") {
          ADD_FAILURE() << name << ": unknown k3739_mode " << v.string;
          ok = false;
        }
      } else if (key == "cosmogenic") {
        if (!v.is_null()) {
          rc.cosmogenic =
              CosmogenicRatios{measured_of(v["solar3836"]), measured_of(v["cosmo3836"])};
        }
      } else if (key == "lambda_b") rc.lambda_b = measured_of(v);
      else if (key == "lambda_e") rc.lambda_e = measured_of(v);
      else if (key == "lambda_ar37") rc.lambda_ar37 = measured_of(v);
      else if (key == "lambda_ar39") rc.lambda_ar39 = measured_of(v);
      else if (key == "abundance_sensitivity") rc.abundance_sensitivity = v.as_number();
      else if (key == "include_decay_error") rc.include_decay_error = v.as_bool();
      else if (key == "use_irradiation_endtime") rc.use_irradiation_endtime = v.as_bool();
      else if (key == "age_units") {
        if (v.as_string() != "Ma") ADD_FAILURE() << name << ": unhandled age_units " << v.string;
      } else {
        ADD_FAILURE() << name << ": unhandled constants key " << key;
        ok = false;
      }
    }
    if (!ok) continue;
    if (rc.cosmogenic) ++n_cosmo;

    std::map<std::string, Measured, std::less<>> rows;
    for (const auto& [key, m] : in["production"].as_object()) rows[key] = measured_of(m);
    auto pr = production_from_rows(rows);
    if (!pr) {
      ADD_FAILURE() << name << ": " << pr.error().what;
      continue;
    }
    const ProductionVariables pv = make_production_variables(*pr);

    std::array<UFloat, 5> n;
    for (const ArgonIsotope iso : kArgonKeys) {
      const std::string key(to_string(iso));
      const g::Json& j = in["isotopes"][key];
      if (!j.is_object()) {
        ADD_FAILURE() << name << ": missing isotope " << key;
        ok = false;
        continue;
      }
      n[index(iso)] = UFloat::variable(j["v"].as_number(), j["e"].as_number(), key);
    }
    if (in["isotopes"].size() != 5u) ADD_FAILURE() << name << ": isotope count";
    if (!ok) continue;
    std::optional<Measured> fixed;
    if (!in["fixed_k3739"].is_null()) fixed = measured_of(in["fixed_k3739"]);

    const auto r = calculate_f(n, in["decay_days"].as_number(), pv, rc, fixed);
    if (!r) {
      ADD_FAILURE() << name << ": " << r.error().what;
      continue;
    }

    std::vector<std::string> want_diags;
    for (const g::Json& d : c["expect_diagnostics"].as_array()) want_diags.push_back(d.as_string());
    if (!want_diags.empty()) ++n_diag;
    EXPECT_EQ(names_of(r->diagnostics), want_diags);

    // legacy_sentinel: the legacy value of an output that C++ leaves absent.
    const g::Json& sentinel = c["legacy_sentinel"];
    if (!sentinel.is_null()) {
      ++n_sentinel;
      for (const auto& [key, v] : sentinel.as_object()) {
        if (c["expected"].contains(key)) ADD_FAILURE() << name << ": sentinel key also expected";
        if (key == "f") {
          EXPECT_FALSE(r->f.has_value()) << "legacy F sentinel " << v["v"].as_number();
          EXPECT_TRUE(std::find(want_diags.begin(), want_diags.end(), "FUndefined") !=
                      want_diags.end());
        } else if (key == "radiogenic_yield") {
          EXPECT_FALSE(r->radiogenic_yield.has_value())
              << "legacy yield sentinel " << v["v"].as_number();
          EXPECT_TRUE(std::find(want_diags.begin(), want_diags.end(), "YieldUndefined") !=
                      want_diags.end());
        } else {
          ADD_FAILURE() << name << ": unhandled legacy_sentinel key " << key;
        }
      }
    }

    auto check = [&](const UFloat& got, const g::Json& w, const std::string& what) {
      g::expect_close(got.nominal(), w["v"].as_number(), t.rtol, t.atol, what + ".v");
      g::expect_close(got.std_dev(), w["e"].as_number(), t.rtol_err, t.atol_err, what + ".e");
    };
    auto check_group = [&](const g::Json& w, const std::map<std::string, const UFloat*>& got,
                           const std::string& group) {
      if (w.size() != got.size()) {
        ADD_FAILURE() << group << fmt(": expected key count ", static_cast<double>(w.size()));
      }
      for (const auto& [key, v] : w.as_object()) {
        const auto it = got.find(key);
        if (it == got.end()) {
          ADD_FAILURE() << name << ": unhandled key " << group << "." << key;
          continue;
        }
        check(*it->second, v, group + "." + key);
      }
    };

    const auto& ifc = r->interference_corrected;
    for (const auto& [key, w] : c["expected"].as_object()) {
      if (key == "f") {
        if (!r->f) {
          ADD_FAILURE() << name << ": f absent";
          continue;
        }
        check(*r->f, w, key);
      } else if (key == "f_err_wo_irrad") {
        g::expect_close(r->f_err_wo_irrad, w.as_number(), t.rtol_err, t.atol_err, key);
      } else if (key == "radiogenic_yield") {
        if (!r->radiogenic_yield) {
          ADD_FAILURE() << name << ": radiogenic_yield absent";
          continue;
        }
        check(*r->radiogenic_yield, w, key);
      } else if (key == "atm40") {
        check(r->atm40, w, key);
      } else if (key == "k40") {
        check(r->k40, w, key);
      } else if (key == "rad40") {
        check(r->rad40, w, key);
      } else if (key == "interference") {
        const auto& x = r->interference;
        check_group(w,
                    {{"k37", &x.k37}, {"k38", &x.k38}, {"k39", &x.k39}, {"ca36", &x.ca36},
                     {"ca37", &x.ca37}, {"ca38", &x.ca38}, {"ca39", &x.ca39}},
                    key);
      } else if (key == "atmospheric") {
        const auto& x = r->atmospheric;
        check_group(
            w, {{"atm36", &x.atm36}, {"atm38", &x.atm38}, {"cl36", &x.cl36}, {"cl38", &x.cl38}},
            key);
      } else if (key == "cosmogenic") {
        if (!r->cosmogenic) {
          ADD_FAILURE() << name << ": cosmogenic absent";
          continue;
        }
        const auto& x = *r->cosmogenic;
        check_group(w,
                    {{"cosmo36", &x.cosmo36}, {"cosmo38", &x.cosmo38},
                     {"noncosmo36", &x.noncosmo36}, {"noncosmo38", &x.noncosmo38}},
                    key);
      } else if (key == "interference_corrected") {
        check_group(w,
                    {{"Ar40", &ifc[0]}, {"Ar39", &ifc[1]}, {"Ar38", &ifc[2]}, {"Ar37", &ifc[3]},
                     {"Ar36", &ifc[4]}},
                    key);
      } else {
        ADD_FAILURE() << name << ": unhandled expected key " << key;
      }
    }
    if (!c["expected"].contains("cosmogenic")) EXPECT_FALSE(r->cosmogenic.has_value());
  }
  EXPECT_EQ(n_legacy, 12u);
  EXPECT_EQ(n_prefs, 12u);
  EXPECT_EQ(n_sentinel, 4u);
  EXPECT_EQ(n_cosmo, 2u);
  EXPECT_EQ(n_diag, 5u);
}
