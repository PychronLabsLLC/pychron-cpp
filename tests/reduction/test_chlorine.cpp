// Chlorine: E12 cl36/cl38 through reduce(), E19 K/Cl and Cl/K, missing Cl
// production and the cl38 == 0 sentinel (spec 3.4, 3.7, Q6, Q16;
// legacy arar_age.py:547-566, argon_calculations.py:468-487).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "golden.hpp"
#include "pychron/reduction/arar_reduction.hpp"
#include "reduce_golden.hpp"

using namespace pychron::reduction;
using pychron::Result;
namespace g = pychron::reduction::golden;

namespace {

using g::diagnostic_names;

// Same nominal, same variables, same partials (bitwise).
void expect_identical(const UFloat& got, const UFloat& want, const std::string& what) {
  EXPECT_EQ(got.nominal(), want.nominal()) << what;
  EXPECT_EQ(got.terms().size(), want.terms().size()) << what;
  if (got.terms().size() != want.terms().size()) return;
  for (std::size_t i = 0; i < got.terms().size(); ++i) {
    EXPECT_EQ(got.terms()[i].id, want.terms()[i].id) << what;
    EXPECT_EQ(got.terms()[i].deriv, want.terms()[i].deriv) << what;
  }
}

void expect_rel(double got, double want, double rtol, const std::string& what) {
  EXPECT_LE(std::fabs(got - want), rtol * std::fabs(want))
      << what << ": got " << ::testing::PrintToString(got) << ", want "
      << ::testing::PrintToString(want);
}

// legacy CalculateFTest-like intensities, no blanks or baselines, Cl3638 > 0,
// 5 years since irradiation.
ProductionRatios chlorine_ratios() {
  ProductionRatios p;
  p.k4039 = {0.0002, 0.00001};
  p.k3839 = {0.013, 0.0001};
  p.k3739 = {0.01, 0.0001};
  p.ca3937 = {0.0007, 0.00001};
  p.ca3837 = {0.00019, 0.0};
  p.ca3637 = {0.00026, 0.00001};
  p.cl3638 = {250.0, 5.0};
  p.ca_k = Measured{2.0, 0.02};
  p.cl_k = Measured{0.1, 0.001};
  return p;
}

ReductionInput chlorine_input(const std::array<Measured, 5>& v, const ProductionRatios& p) {
  ReductionInput in;
  for (const ArgonIsotope iso : kArgonKeys) {
    MeasuredSignal m;
    m.intercept = v[index(iso)];
    in.isotopes[index(iso)] = make_signal(iso, m);
  }
  in.constants = constants_preset(ConstantsPreset::Legacy);
  in.production = make_production_variables(p);
  in.irradiation.decay_days = 1825.0;
  in.j = make_j(Flux{{0.001, 1e-6}, 5e-7, std::nullopt});
  in.position_jerr = 5e-7;
  return in;
}

const std::array<Measured, 5> kIntensities{
    {{1000.0, 1.0}, {100.0, 0.5}, {10.0, 0.05}, {5.0, 0.025}, {2.0, 0.01}}};

ReductionInput chlorine_input() { return chlorine_input(kIntensities, chlorine_ratios()); }

bool has(const std::vector<Diagnostic>& d, Diagnostic x) {
  return std::find(d.begin(), d.end(), x) != d.end();
}

}  // namespace

// ---- E19 K/Cl (arar_age.py:547-558) -----------------------------------------

TEST(Chlorine, KClUsesResidualCl38AndClK) {
  const ReductionInput in = chlorine_input();
  const auto r = reduce(in);
  ASSERT_TRUE(r) << r.error().what;
  ASSERT_TRUE(r->kcl.has_value());
  ASSERT_TRUE(r->clk.has_value());
  const UFloat& k39 = r->f.interference.k39;
  const UFloat& cl38 = r->f.atmospheric.cl38;
  ASSERT_TRUE(in.production.cl_k.has_value());
  EXPECT_GT(cl38.nominal(), 0.0);
  // Legacy order k / cl * (1 / Cl_K); Cl_K is the analysis' production
  // variable, so its error propagates.
  expect_identical(*r->kcl, k39 / cl38 * (1.0 / *in.production.cl_k), "kcl");
  expect_rel(r->kcl->nominal(), k39.nominal() / cl38.nominal() / in.production.cl_k->nominal(),
             1e-14, "kcl == k39 / cl38 / Cl_K");
  expect_identical(*r->clk, 1.0 / *r->kcl, "clk");
  EXPECT_NE(r->kcl->derivative(in.production.cl_k->variable_id()), 0.0);
  EXPECT_NE(r->kcl->derivative(in.production.cl3638.variable_id()), 0.0);  // via cl38
  EXPECT_FALSE(has(r->diagnostics, Diagnostic::KClUndefined));
  EXPECT_TRUE(r->diagnostics.empty()) << ::testing::PrintToString(diagnostic_names(r->diagnostics));
}

TEST(Chlorine, MissingClProduction) {
  ProductionRatios p = chlorine_ratios();
  p.cl3638 = {0.0, 0.0};  // production_from_rows: a missing key is {0, 0}
  p.cl_k.reset();
  const ReductionInput in = chlorine_input(kIntensities, p);
  const auto r = reduce(in);
  ASSERT_TRUE(r) << r.error().what;
  const AtmosphericComponents& a = r->f.atmospheric;
  const InterferenceComponents& ic = r->f.interference;
  // m = 0: cl36 is exactly 0 with no error, cl38 is the residual 38 (legacy
  // keeps it, argon_calculations.py:484-485).
  EXPECT_EQ(a.cl36.nominal(), 0.0);
  EXPECT_EQ(a.cl36.std_dev(), 0.0);
  const UFloat& a38 = r->corrected[index(ArgonIsotope::Ar38)];
  expect_identical(a.cl38, a38 - a.atm38 - ic.k38 - ic.ca38, "residual cl38");
  EXPECT_GT(a.cl38.nominal(), 0.0);
  // Cl_K missing -> factor 1.
  ASSERT_TRUE(r->kcl && r->clk);
  expect_identical(*r->kcl, ic.k39 / a.cl38, "kcl factor 1");
  expect_identical(*r->clk, 1.0 / *r->kcl, "clk");

  // Cl_K nominally 0 -> factor 1 as well (spec E19; legacy `or 1.0`).
  ReductionInput zero = chlorine_input(kIntensities, p);
  zero.production.cl_k = UFloat(0.0);
  const auto rz = reduce(zero);
  ASSERT_TRUE(rz && rz->kcl);
  expect_identical(*rz->kcl, rz->f.interference.k39 / rz->f.atmospheric.cl38, "zero Cl_K");
}

TEST(Chlorine, Cl38ZeroUndefined) {
  // No Cl3638, K3839, Ca3837, Ca3637: atm38 = atm3836 * a36 and cl38 =
  // a38 - atm38 is exactly 0 when a38 is that product (golden
  // reduce_cl38_zero, pipeline zero_k39).
  ProductionRatios p = chlorine_ratios();
  p.cl3638 = {0.0, 0.0};
  p.k3839 = {0.0, 0.0};
  p.ca3837 = {0.0, 0.0};
  p.ca3637 = {0.0, 0.0};
  const ReductionConstants c = constants_preset(ConstantsPreset::Legacy);
  std::array<Measured, 5> v = kIntensities;
  v[index(ArgonIsotope::Ar38)] = {c.atm4036.value / c.atm4038.value * 2.0, 0.05};
  const auto r = reduce(chlorine_input(v, p));
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->f.atmospheric.cl38.nominal(), 0.0);
  EXPECT_GT(r->f.atmospheric.cl38.std_dev(), 0.0);
  // Legacy ZeroDivisionError -> kcl = 0 sentinel (D3: absent + diagnostic).
  EXPECT_FALSE(r->kcl.has_value());
  EXPECT_FALSE(r->clk.has_value());
  EXPECT_EQ(diagnostic_names(r->diagnostics), (std::vector<std::string>{"KClUndefined"}));
  EXPECT_TRUE(r->kca && r->cak);  // K/Ca is unaffected
  EXPECT_TRUE(r->ages.has_value());
}

// kcl == 0 with cl38 != 0 (k39 == 0): legacy 1 / kcl raises after kcl was
// assigned. As for K/Ca (Task 11 ruling): kcl kept as computed (0 +- e), clk
// absent, KClUndefined.
TEST(Chlorine, KClZeroKeepsKclWithoutClk) {
  ProductionRatios p = chlorine_ratios();
  p.ca3937 = {0.0, 0.0};  // ca39 = 0, so k39 = a39 = 0 exactly
  std::array<Measured, 5> v = kIntensities;
  v[index(ArgonIsotope::Ar39)] = {0.0, 0.5};
  const auto r = reduce(chlorine_input(v, p));
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(r->f.interference.k39.nominal(), 0.0);
  EXPECT_NE(r->f.atmospheric.cl38.nominal(), 0.0);
  ASSERT_TRUE(r->kcl.has_value());
  EXPECT_EQ(r->kcl->nominal(), 0.0);
  EXPECT_GT(r->kcl->std_dev(), 0.0);
  EXPECT_FALSE(r->clk.has_value());
  EXPECT_EQ(diagnostic_names(r->diagnostics),
            (std::vector<std::string>{"FUndefined", "KCaUndefined", "KClUndefined"}));
}

// ---- E12: decay_days drives m -----------------------------------------------

TEST(Chlorine, DecayDaysScalesCl36) {
  ReductionInput in1 = chlorine_input();
  ReductionInput in2 = chlorine_input();
  in1.irradiation.decay_days = 365.0;
  in2.irradiation.decay_days = 730.0;
  const auto r1 = reduce(in1);
  const auto r2 = reduce(in2);
  ASSERT_TRUE(r1 && r2);
  const AtmosphericComponents& a1 = r1->f.atmospheric;
  const AtmosphericComponents& a2 = r2->f.atmospheric;
  // cl36 = m cl38, so m = cl36 / cl38.
  const double m1 = a1.cl36.nominal() / a1.cl38.nominal();
  const double m2 = a2.cl36.nominal() / a2.cl38.nominal();
  const ReductionConstants& c = in1.constants;
  const double cl3638 = in1.production.cl3638.nominal();
  expect_rel(m1, cl3638 * c.lambda_cl36.value * 365.0, 1e-13, "m(365)");
  expect_rel(m2 / m1, 2.0, 1e-13, "m doubles");
  // atm36 per E12 on the nominals: (a36 - ca36 - m (a38 - k38 - ca38)) / (1 - m r3836).
  const double r3836 = c.atm4036.value / c.atm4038.value;
  for (const auto& [r, m, what] : {std::tuple{&*r1, m1, "365"}, std::tuple{&*r2, m2, "730"}}) {
    const InterferenceComponents& ic = r->f.interference;
    const double a36 = r->corrected[index(ArgonIsotope::Ar36)].nominal();
    const double a38 = r->corrected[index(ArgonIsotope::Ar38)].nominal();
    const double want = (a36 - ic.ca36.nominal() -
                         m * (a38 - ic.k38.nominal() - ic.ca38.nominal())) /
                        (1.0 - m * r3836);
    expect_rel(r->f.atmospheric.atm36.nominal(), want, 1e-12, std::string("atm36 ") + what);
  }
  EXPECT_LT(a2.atm36.nominal(), a1.atm36.nominal());  // more cl36 removed from 36
  EXPECT_GT(a2.cl36.nominal(), a1.cl36.nominal());
  ASSERT_TRUE(r1->kcl && r2->kcl);
  EXPECT_NE(r1->kcl->nominal(), r2->kcl->nominal());  // via cl38
}

// ---- Golden: chlorine.json and the chlorine cases of pipeline.json ----------

namespace {

// One chlorine.json calculate_f case; every key is read.
void run_calculate_f_case(const g::Json& c) {
  const std::string name = c["name"].string;
  const g::Json& in = c["inputs"];
  g::known_keys(in, {"function", "constants", "production", "isotopes", "decay_days", "fixed_k3739"},
                name + " inputs");
  if (!c["expect_error"].is_null()) ADD_FAILURE() << name << ": unhandled expect_error";
  if (!c["legacy_sentinel"].is_null()) ADD_FAILURE() << name << ": unhandled legacy_sentinel";
  ReductionConstants rc;
  if (!g::constants_of(in["constants"], rc, name)) return;
  std::map<std::string, Measured, std::less<>> rows;
  for (const auto& [key, m] : in["production"].as_object()) rows[key] = g::measured_of(m);
  const auto pr = production_from_rows(rows);
  if (!pr) {
    ADD_FAILURE() << name << ": " << pr.error().what;
    return;
  }
  std::array<UFloat, 5> n;
  EXPECT_EQ(in["isotopes"].size(), 5u) << name;
  for (const ArgonIsotope iso : kArgonKeys) {
    const std::string key(to_string(iso));
    const g::Json& j = in["isotopes"][key];
    if (!j.is_object()) {
      ADD_FAILURE() << name << ": missing isotope " << key;
      return;
    }
    n[index(iso)] = UFloat::variable(j["v"].as_number(), j["e"].as_number(), key);
  }
  std::optional<Measured> fixed;
  if (!in["fixed_k3739"].is_null()) fixed = g::measured_of(in["fixed_k3739"]);
  const auto r = calculate_f(n, in["decay_days"].as_number(), make_production_variables(*pr), rc,
                             fixed);
  if (!r) {
    ADD_FAILURE() << name << ": " << r.error().what;
    return;
  }
  std::vector<std::string> want_diags;
  for (const g::Json& d : c["expect_diagnostics"].as_array()) want_diags.push_back(d.as_string());
  EXPECT_EQ(diagnostic_names(r->diagnostics), want_diags);
  g::check_f(c["expected"], *r, g::tol_of(c));
}

}  // namespace

TEST(Chlorine, Golden) {
  std::size_t n_calculate_f = 0, n_reduce = 0, n_pipeline = 0;
  g::PipelineCounts n;
  const g::Json doc = g::load("chlorine.json");
  ASSERT_EQ(doc["cases"].size(), 22u);
  for (const g::Json& c : doc["cases"].as_array()) {
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    g::known_keys(c,
                  {"name", "source", "inputs", "expected", "tol", "legacy_sentinel",
                   "expect_diagnostics", "expect_error"},
                  name);
    const std::string& fn = c["inputs"]["function"].as_string();
    if (fn == "calculate_f") {
      ++n_calculate_f;
      run_calculate_f_case(c);
    } else if (fn == "reduce") {
      ++n_reduce;
      g::run_pipeline_case(c, n);
    } else {
      ADD_FAILURE() << name << ": unhandled function " << fn;
    }
  }
  const g::Json pipeline = g::load("pipeline.json");
  for (const g::Json& c : pipeline["cases"].as_array()) {
    const std::string name = c["name"].string;
    if (!g::is_chlorine(name)) continue;
    SCOPED_TRACE(name);
    ++n_pipeline;
    g::run_pipeline_case(c, n);
  }
  // decay_days 0/30/365/3650, missing_cl3638, ratio_errors x 2 presets.
  EXPECT_EQ(n_calculate_f, 12u);
  // cl3638, cl3638_decay_days_3650, missing_cl_production, cl_k_zero, cl38_zero x 2.
  EXPECT_EQ(n_reduce, 10u);
  EXPECT_EQ(n_pipeline, 4u);  // chlorine_cl3638, chlorine_with_segments x 2
  EXPECT_EQ(n.legacy, 7u);
  EXPECT_EQ(n.prefs, 7u);
  EXPECT_EQ(n.kcl, 12u);           // every reduce case but cl38_zero
  EXPECT_EQ(n.kcl_undefined, 2u);  // cl38_zero
  EXPECT_EQ(n.sentinel, 2u);       // cl38_zero: kcl, clk
  EXPECT_EQ(n.diag, 2u);
  EXPECT_EQ(n.components, 14u);
  EXPECT_EQ(n.error, 0u);
  EXPECT_EQ(n.cosmo, 0u);
  EXPECT_EQ(n.no_j, 0u);
  EXPECT_EQ(n.lambda_override, 0u);
}
