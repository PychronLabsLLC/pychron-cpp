// Age equation, J variants and error budget (spec 3.6, E16-E18; Q4, Q6, Q13;
// Review Focus 2).
#include <gtest/gtest.h>

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

std::vector<const UFloat::Term*> terms_tagged(const UFloat& x, std::string_view tag) {
  std::vector<const UFloat::Term*> out;
  for (const UFloat::Term& t : x.terms()) {
    if (tag_name(t.tag) == tag) out.push_back(&t);
  }
  return out;
}

void expect_rel(double got, double want, double rtol, const std::string& what) {
  EXPECT_LE(std::fabs(got - want), rtol * std::fabs(want))
      << what << ": got " << ::testing::PrintToString(got) << ", want "
      << ::testing::PrintToString(want);
}

// Legacy ArArConstants trait values (lambda_b + lambda_e = 5.543e-10 / a).
ReductionConstants legacy_constants() { return constants_preset(ConstantsPreset::Legacy); }

constexpr double kLambda = 4.962e-10 + 5.81e-11;

using g::constants_of;
using g::known_keys;
using g::measured_of;
using g::units_of;

}  // namespace

// legacy AgeEquationTest test_age_for_known_jf, arar_constants.py:143-170.
TEST(Age, KnownValueAndUnits) {
  const UFloat j = UFloat::variable(0.001, 1e-6, "J");
  const UFloat f = UFloat::variable(10.0, 0.01, "F");
  const double t_years = std::log(1.0 + 0.001 * 10.0) / kLambda;
  const std::array<std::pair<AgeUnits, double>, 4> units{
      {{AgeUnits::a, 1.0}, {AgeUnits::ka, 1e-3}, {AgeUnits::Ma, 1e-6}, {AgeUnits::Ga, 1e-9}}};
  for (const auto& [u, scale] : units) {
    ReductionConstants c = legacy_constants();
    c.age_units = u;
    const auto r = age_equation(j, f, c);
    ASSERT_TRUE(r) << r.error().what;
    expect_rel(r->nominal(), t_years * scale, 1e-12, fmt("age scale ", scale));
    EXPECT_EQ(age_scale(AgeUnits::a, u), scale);
  }
  // 17.95 Ma (age.json age/known_jf).
  const auto ma = age_equation(j, f, legacy_constants());
  ASSERT_TRUE(ma);
  expect_rel(ma->nominal(), 17.951165168984467, 1e-12, "known_jf Ma");
  expect_rel(ma->std_dev(), 0.025260895686345902, 1e-10, "known_jf Ma err");
  EXPECT_EQ(age_scale(AgeUnits::Ma, AgeUnits::a), 1e6);
  EXPECT_EQ(age_scale(AgeUnits::Ga, AgeUnits::Ma), 1e9 * 1e-6);
}

// legacy scale_age: value * scalar(current) * targetscalar(target).
TEST(Age, ScaleMatchesLegacyScaleAge) {
  const g::Json doc = g::load("constants.json");
  std::size_t seen = 0;
  for (const g::Json& c : doc["cases"].as_array()) {
    const std::string& name = c["name"].as_string();
    if (!name.starts_with("scale_age/")) continue;
    ++seen;
    SCOPED_TRACE(name);
    AgeUnits from{}, to{};
    if (!units_of(c["inputs"]["current"].as_string(), from) ||
        !units_of(c["inputs"]["target"].as_string(), to)) {
      ADD_FAILURE() << name << ": unknown units";
      continue;
    }
    g::expect_close(c["inputs"]["value"].as_number() * age_scale(from, to),
                    c["expected"]["value"].as_number(), 1e-12, 0.0, name);
  }
  EXPECT_EQ(seen, 16u);
}

// legacy AgeEquationEdgeCasesTest test_include_decay_error_increases_uncertainty;
// argon_calculations.py:622-623.
TEST(Age, DecayErrorOnlyWhenRequested) {
  const UFloat j = UFloat::variable(0.001, 1e-6, "J");
  const UFloat f = UFloat::variable(10.0, 0.01, "F");
  ReductionConstants c = legacy_constants();
  const auto without = age_equation(j, f, c);
  ASSERT_TRUE(without) << without.error().what;
  EXPECT_TRUE(terms_tagged(*without, "lambda_k").empty());
  EXPECT_EQ(without->terms().size(), 2u);  // J and F only

  c.include_decay_error = true;
  const auto with = age_equation(j, f, c);
  ASSERT_TRUE(with) << with.error().what;
  const auto lk = terms_tagged(*with, "lambda_k");
  ASSERT_EQ(lk.size(), 1u);
  // sigma of lambda_b + lambda_e in quadrature; dt/dlambda = -t / lambda.
  expect_rel(lk[0]->sigma, std::hypot(9.3e-13, 1.6e-13), 1e-12, "lambda_k sigma");
  expect_rel(lk[0]->deriv, -with->nominal() / kLambda, 1e-12, "dt/dlambda");
  EXPECT_EQ(with->nominal(), without->nominal());
  EXPECT_GT(with->std_dev(), without->std_dev());
  EXPECT_EQ(with->derivative(j.variable_id()), without->derivative(j.variable_id()));
  EXPECT_EQ(with->derivative(f.variable_id()), without->derivative(f.variable_id()));

  // A fresh lambda_k variable per call (spec Q1, legacy property access).
  const auto again = age_equation(j, f, c);
  ASSERT_TRUE(again);
  ASSERT_EQ(terms_tagged(*again, "lambda_k").size(), 1u);
  EXPECT_NE(terms_tagged(*again, "lambda_k")[0]->id, lk[0]->id);
}

// argon_calculations.py:614 (`if not lambda_k`), dvc/dvc.py:2303-2305 (`if lk:`).
TEST(Age, LambdaKOverride) {
  const UFloat j = UFloat::variable(0.001, 1e-6, "J");
  const UFloat f = UFloat::variable(10.0, 0.01, "F");
  ReductionConstants c = legacy_constants();
  c.include_decay_error = true;
  const auto r = age_equation(j, f, c, Measured{5.5305e-10, 1.1e-12});
  ASSERT_TRUE(r) << r.error().what;
  expect_rel(r->nominal(), std::log(1.01) / 5.5305e-10 * 1e-6, 1e-12, "override nominal");
  const auto lk = terms_tagged(*r, "lambda_k");
  ASSERT_EQ(lk.size(), 1u);
  EXPECT_EQ(lk[0]->sigma, 1.1e-12);
  expect_rel(lk[0]->deriv, -r->nominal() / 5.5305e-10, 1e-12, "dt/dlambda_k");

  // Override replaces even zero-error constants.
  ReductionConstants exact = c;
  exact.lambda_b.error = 0.0;
  exact.lambda_e.error = 0.0;
  const auto r2 = age_equation(j, f, exact, Measured{5.5305e-10, 1.1e-12});
  ASSERT_TRUE(r2);
  EXPECT_EQ(r2->nominal(), r->nominal());
  EXPECT_EQ(terms_tagged(*r2, "lambda_k").size(), 1u);

  // 0 +- 0 is falsy: ignored, lambda_b + lambda_e used.
  const auto zero = age_equation(j, f, c, Measured{0.0, 0.0});
  const auto none = age_equation(j, f, c);
  ASSERT_TRUE(zero);
  ASSERT_TRUE(none);
  EXPECT_EQ(zero->nominal(), none->nominal());
  expect_rel(zero->std_dev(), none->std_dev(), 1e-15, "zero override std");

  // Without decay error the override enters as a nominal only.
  c.include_decay_error = false;
  const auto nominal_only = age_equation(j, f, c, Measured{5.5305e-10, 1.1e-12});
  ASSERT_TRUE(nominal_only);
  EXPECT_EQ(nominal_only->nominal(), r->nominal());
  EXPECT_TRUE(terms_tagged(*nominal_only, "lambda_k").empty());
}

// spec Q6 / D3: no 0 +- 0 sentinel; reduce maps the error to AgeUndefined.
TEST(Age, NonPositiveArgumentErrors) {
  const ReductionConstants c = legacy_constants();
  for (const double f : {-2.0, -1.0}) {
    const auto r = age_equation(UFloat(1.0), UFloat(f), c);
    ASSERT_FALSE(r) << fmt("F = ", f);
    EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
    EXPECT_EQ(r.error().what.rfind("reduction: ", 0), 0u) << r.error().what;
    EXPECT_NE(r.error().what.find("1 + J F"), std::string::npos) << r.error().what;
  }
  // make_age_set reports the same condition as an absent set, not an error.
  const std::array<VariableId, 7> none{};
  const auto s = detail::make_age_set(UFloat(1.0), 0.0, UFloat(-2.0), c, std::nullopt, none);
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_FALSE(s->has_value());
  // 1 + J F just above zero is defined.
  EXPECT_TRUE(age_equation(UFloat(1.0), UFloat(-0.999), c));
}

TEST(Age, InvalidConstantsError) {
  const UFloat j = UFloat::variable(0.001, 1e-6, "J");
  const UFloat f = UFloat::variable(10.0, 0.01, "F");
  ReductionConstants zero;  // value-initialised: lambda_b + lambda_e == 0
  auto r = age_equation(j, f, zero);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().what.rfind("reduction: ", 0), 0u);
  EXPECT_NE(r.error().what.find("lambda"), std::string::npos) << r.error().what;
  const std::array<VariableId, 7> none{};
  EXPECT_FALSE(detail::make_age_set(j, 0.0, f, zero, std::nullopt, none));

  ReductionConstants c = legacy_constants();
  c.lambda_b.error = -1.0;
  c.include_decay_error = true;
  EXPECT_FALSE(age_equation(j, f, c));
  c = legacy_constants();
  c.lambda_e.value = std::nan("");
  EXPECT_FALSE(age_equation(j, f, c));
  c = legacy_constants();
  EXPECT_FALSE(age_equation(j, f, c, Measured{HUGE_VAL, 0.0}));
  EXPECT_FALSE(age_equation(UFloat(std::nan("")), f, c));
  EXPECT_FALSE(detail::make_age_set(j, -1e-6, f, c, std::nullopt, none));
  EXPECT_FALSE(detail::make_age_set(j, std::nan(""), f, c, std::nullopt, none));
}

// Review Focus 2; legacy arar_age.py:658-686.
TEST(Age, JVariants) {
  const UFloat j = UFloat::variable(0.001, 1e-6, "J");
  const UFloat a = UFloat::variable(50.0, 0.1, "Ar40");
  const UFloat b = UFloat::variable(5.0, 0.02, "Ar39");
  const UFloat f = a / b;  // F with two variables of its own
  const double position_jerr = 3e-6;
  const ReductionConstants c = legacy_constants();  // include_decay_error false
  const std::array<VariableId, 7> none{};
  const auto s = detail::make_age_set(j, position_jerr, f, c, std::nullopt, none);
  ASSERT_TRUE(s) << s.error().what;
  ASSERT_TRUE(s->has_value());
  const AgeSet& ages = **s;
  const VariableId jid = j.variable_id();
  ASSERT_NE(jid, 0u);

  // age: analytical error only (J_no_err is exact, so it has no term).
  EXPECT_EQ(ages.age.derivative(jid), 0.0);
  EXPECT_TRUE(terms_tagged(ages.age, "J_no_err").empty());
  EXPECT_TRUE(terms_tagged(ages.age, "Position").empty());
  EXPECT_EQ(ages.age.terms().size(), 2u);

  // age_w_j_err: dt/dJ = F / (lambda (1 + J F)) * scale.
  const double F = f.nominal();
  const double dtdj = F / (kLambda * (1.0 + 0.001 * F)) * 1e-6;
  expect_rel(ages.age_w_j_err.derivative(jid), dtdj, 1e-12, "dt/dJ");
  EXPECT_TRUE(terms_tagged(ages.age_w_j_err, "Position").empty());
  EXPECT_EQ(ages.age_w_j_err.terms().size(), 3u);

  // age_w_position_err: one independent Position variable, sigma position_jerr.
  EXPECT_EQ(ages.age_w_position_err.derivative(jid), 0.0);
  const auto pos = terms_tagged(ages.age_w_position_err, "Position");
  ASSERT_EQ(pos.size(), 1u);
  EXPECT_EQ(pos[0]->sigma, position_jerr);
  EXPECT_NE(pos[0]->id, jid);
  expect_rel(pos[0]->deriv, dtdj, 1e-12, "dt/dPosition");
  EXPECT_EQ(ages.age_w_position_err.terms().size(), 3u);

  // All three on the same F: same nominal and identical F partials.
  for (const UFloat* x : {&ages.age_w_j_err, &ages.age_w_position_err}) {
    EXPECT_EQ(x->nominal(), ages.age.nominal());
    for (const UFloat::Term& t : f.terms()) {
      EXPECT_NE(ages.age.derivative(t.id), 0.0);
      EXPECT_EQ(x->derivative(t.id), ages.age.derivative(t.id)) << tag_name(t.tag);
    }
  }
  // The age equation agrees with the variant built from the supplied J.
  const auto direct = age_equation(j, f, c);
  ASSERT_TRUE(direct);
  EXPECT_EQ(direct->nominal(), ages.age_w_j_err.nominal());
  EXPECT_EQ(direct->derivative(jid), ages.age_w_j_err.derivative(jid));

  // position_jerr 0: Position is exact, so the position variant equals age.
  const auto s0 = detail::make_age_set(j, 0.0, f, c, std::nullopt, none);
  ASSERT_TRUE(s0 && s0->has_value());
  EXPECT_TRUE(terms_tagged((*s0)->age_w_position_err, "Position").empty());
  EXPECT_EQ((*s0)->age_w_position_err.std_dev(), (*s0)->age.std_dev());

  // Two analyses sharing one J UFloat: cov(age_w_j_err) = dA/dJ dB/dJ sigma_J^2,
  // while the J-free ages stay uncorrelated.
  const UFloat f2 = UFloat::variable(20.0, 0.05, "F2");
  const auto s2 = detail::make_age_set(j, position_jerr, f2, c, std::nullopt, none);
  ASSERT_TRUE(s2 && s2->has_value());
  const double dA = ages.age_w_j_err.derivative(jid);
  const double dB = (*s2)->age_w_j_err.derivative(jid);
  ASSERT_NE(dB, 0.0);
  expect_rel(covariance(ages.age_w_j_err, (*s2)->age_w_j_err), dA * dB * 1e-6 * 1e-6, 1e-12,
             "cov via J");
  EXPECT_EQ(covariance(ages.age, (*s2)->age), 0.0);
  EXPECT_EQ(covariance(ages.age_w_position_err, (*s2)->age_w_position_err), 0.0);
}

// With decay error each variant reads lambda_k afresh (legacy age_equation
// reads arar_constants.lambda_k per call), unless lambda_k_total overrides it
// (one variable set on the constants, dvc.py:2303-2305).
TEST(Age, JVariantsDecayErrorVariables) {
  const UFloat j = UFloat::variable(0.001, 1e-6, "J");
  const UFloat f = UFloat::variable(10.0, 0.01, "F");
  ReductionConstants c = legacy_constants();
  c.include_decay_error = true;
  const std::array<VariableId, 7> none{};
  const auto s = detail::make_age_set(j, 2e-6, f, c, std::nullopt, none);
  ASSERT_TRUE(s && s->has_value());
  const AgeSet& ages = **s;
  const auto l1 = terms_tagged(ages.age, "lambda_k");
  const auto l2 = terms_tagged(ages.age_w_j_err, "lambda_k");
  const auto l3 = terms_tagged(ages.age_w_position_err, "lambda_k");
  ASSERT_EQ(l1.size(), 1u);
  ASSERT_EQ(l2.size(), 1u);
  ASSERT_EQ(l3.size(), 1u);
  EXPECT_NE(l1[0]->id, l2[0]->id);
  EXPECT_NE(l1[0]->id, l3[0]->id);
  EXPECT_NE(l2[0]->id, l3[0]->id);

  const auto o = detail::make_age_set(j, 2e-6, f, c, Measured{5.5305e-10, 1.1e-12}, none);
  ASSERT_TRUE(o && o->has_value());
  const auto o1 = terms_tagged((*o)->age, "lambda_k");
  const auto o2 = terms_tagged((*o)->age_w_j_err, "lambda_k");
  const auto o3 = terms_tagged((*o)->age_w_position_err, "lambda_k");
  ASSERT_EQ(o1.size(), 1u);
  ASSERT_EQ(o2.size(), 1u);
  ASSERT_EQ(o3.size(), 1u);
  EXPECT_EQ(o1[0]->id, o2[0]->id);
  EXPECT_EQ(o1[0]->id, o3[0]->id);
  EXPECT_EQ(o1[0]->sigma, 1.1e-12);
}

// spec Q13 / E18.
TEST(Age, ErrorBudgetWithoutIrrad) {
  ProductionRatios pr;
  pr.ca3937 = {0.0007, 0.000007};
  pr.k3739 = {0.01, 0.0001};
  pr.k3839 = {0.013, 0.00013};
  pr.ca3637 = {0.00026, 0.0000026};
  pr.ca3837 = {0.00019, 0.0000019};
  pr.k4039 = {0.0002, 0.000002};
  pr.cl3638 = {250.0, 5.0};
  pr.ca_k = Measured{1.96, 0.1};
  const ProductionVariables p = make_production_variables(pr);
  const std::array<UFloat, 5> n{
      UFloat::variable(1000.0, 1.0, "Ar40"), UFloat::variable(100.0, 0.5, "Ar39"),
      UFloat::variable(10.0, 0.05, "Ar38"), UFloat::variable(5.0, 0.025, "Ar37"),
      UFloat::variable(2.0, 0.01, "Ar36")};
  ReductionConstants c = legacy_constants();
  c.lambda_cl36 = {6.308e-9, 1e-11};
  c.include_decay_error = true;
  const auto fr = calculate_f(n, 365.0, p, c);
  ASSERT_TRUE(fr) << fr.error().what;
  ASSERT_TRUE(fr->f.has_value());
  const UFloat j = UFloat::variable(0.001, 1e-6, "J");
  const std::array<VariableId, 7> ids = p.interference_ids();
  const auto s = detail::make_age_set(j, 2e-6, *fr->f, c, std::nullopt, ids);
  ASSERT_TRUE(s && s->has_value()) << (s ? "" : s.error().what);
  const AgeSet& ages = **s;
  EXPECT_EQ(ages.age_err_wo_irrad, std_dev_excluding(ages.age, ids));
  EXPECT_EQ(ages.age_err_wo_j_irrad, ages.age_err_wo_irrad);
  EXPECT_LT(ages.age_err_wo_irrad, ages.age.std_dev());
  EXPECT_GT(ages.age_err_wo_irrad, 0.0);

  // Independent recomputation by tag: every interference ratio is excluded,
  // nothing else (not lambda_k, trapped_4036 or atm3836).
  double v = 0.0;
  std::size_t excluded = 0;
  for (const UFloat::Term& t : ages.age.terms()) {
    const std::string_view tag = tag_name(t.tag);
    if (tag == "K4039" || tag == "K3839" || tag == "K3739" || tag == "Ca3937" ||
        tag == "Ca3837" || tag == "Ca3637" || tag == "Cl3638") {
      ++excluded;
      continue;
    }
    v += (t.deriv * t.sigma) * (t.deriv * t.sigma);
  }
  EXPECT_EQ(excluded, 7u);
  expect_rel(ages.age_err_wo_irrad, std::sqrt(v), 1e-14, "wo_irrad by tag");
  EXPECT_EQ(terms_tagged(ages.age, "lambda_k").size(), 1u);
}

TEST(Age, Golden) {
  const g::Json doc = g::load("age.json");
  const g::Json& cases = doc["cases"];
  ASSERT_EQ(cases.size(), 28u);
  std::size_t n_legacy = 0, n_prefs = 0, n_error = 0, n_values = 0, n_override = 0;
  std::map<std::string, std::size_t> units_seen;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const g::Json& c = cases[i];
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    known_keys(c,
               {"name", "source", "inputs", "expected", "tol", "legacy_sentinel",
                "expect_diagnostics", "expect_error"},
               name + " case");
    const g::Json& in = c["inputs"];
    known_keys(in, {"constants", "f", "j", "lambda_k_total"}, name + " inputs");
    if (name.ends_with("@legacy")) ++n_legacy;
    else if (name.ends_with("@legacy_preferences")) ++n_prefs;
    if (c["expect_diagnostics"].size() != 0u) {
      ADD_FAILURE() << name << ": unhandled expect_diagnostics";
    }
    const g::Tol t = g::tol_of(c);

    ReductionConstants rc;
    if (!constants_of(in["constants"], rc, name)) continue;
    std::optional<Measured> lk;
    if (!in["lambda_k_total"].is_null()) {
      lk = measured_of(in["lambda_k_total"]);
      ++n_override;
    }
    const Measured jm = measured_of(in["j"]);
    const Measured fm = measured_of(in["f"]);
    const UFloat j = UFloat::variable(jm.value, jm.error, "J");
    const UFloat f = UFloat::variable(fm.value, fm.error, "F");
    const auto r = age_equation(j, f, rc, lk);

    if (!c["expect_error"].is_null()) {
      ++n_error;
      const std::string want = c["expect_error"].as_string();
      if (r) {
        ADD_FAILURE() << name << ": expected error containing '" << want << "', got age "
                      << r->nominal();
        continue;
      }
      EXPECT_NE(r.error().what.find(want), std::string::npos) << r.error().what;
      EXPECT_EQ(r.error().what.rfind("reduction: ", 0), 0u);
      // legacy_sentinel: legacy returned age = 0 +- 0; C++ leaves it absent
      // (D3) and reduce() raises AgeUndefined.
      const g::Json& sentinel = c["legacy_sentinel"];
      if (sentinel.is_null()) ADD_FAILURE() << name << ": error case without legacy_sentinel";
      for (const auto& [key, v] : sentinel.as_object()) {
        if (key != "age") {
          ADD_FAILURE() << name << ": unhandled legacy_sentinel key " << key;
          continue;
        }
        EXPECT_EQ(v["v"].as_number(), 0.0);
        EXPECT_EQ(v["e"].as_number(), 0.0);
      }
      const std::array<VariableId, 7> none{};
      const auto s = detail::make_age_set(j, 0.0, f, rc, lk, none);
      ASSERT_TRUE(s) << s.error().what;
      EXPECT_FALSE(s->has_value());
      if (c["expected"].size() != 0u) ADD_FAILURE() << name << ": error case with expected";
      continue;
    }
    if (!c["legacy_sentinel"].is_null()) ADD_FAILURE() << name << ": sentinel without error";
    if (!r) {
      ADD_FAILURE() << name << ": " << r.error().what;
      continue;
    }
    ++n_values;
    ++units_seen[in["constants"]["age_units"].string];
    for (const auto& [key, w] : c["expected"].as_object()) {
      if (key != "age") {
        ADD_FAILURE() << name << ": unhandled expected key " << key;
        continue;
      }
      g::expect_close(r->nominal(), w["v"].as_number(), t.rtol, t.atol, "age.v");
      g::expect_close(r->std_dev(), w["e"].as_number(), t.rtol_err, t.atol_err, "age.e");
    }
    // make_age_set's age_w_j_err is the age equation on the supplied J.
    const std::array<VariableId, 7> none{};
    const auto s = detail::make_age_set(j, 0.0, f, rc, lk, none);
    ASSERT_TRUE(s) << s.error().what;
    ASSERT_TRUE(s->has_value());
    g::expect_close((*s)->age_w_j_err.nominal(), c["expected"]["age"]["v"].as_number(), t.rtol,
                    t.atol, "age_w_j_err.v");
    g::expect_close((*s)->age_w_j_err.std_dev(), c["expected"]["age"]["e"].as_number(),
                    t.rtol_err, t.atol_err, "age_w_j_err.e");
  }
  EXPECT_EQ(n_legacy, 14u);
  EXPECT_EQ(n_prefs, 14u);
  EXPECT_EQ(n_error, 4u);
  EXPECT_EQ(n_values, 24u);
  EXPECT_EQ(n_override, 6u);
  EXPECT_EQ(units_seen.size(), 4u);
}

// correlation.json "age_equation_pair" cases: one J UFloat shared by two age
// equations (Review Focus 2). The reduce_pair cases belong to reduce() (Task 11).
TEST(Age, SharedJCorrelationGolden) {
  const g::Json doc = g::load("correlation.json");
  const g::Json& cases = doc["cases"];
  ASSERT_EQ(cases.size(), 5u);
  std::size_t n_pair = 0, n_reduce = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const g::Json& c = cases[i];
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    const g::Json& in = c["inputs"];
    const std::string& function = in["function"].as_string();
    if (function == "reduce_pair") {
      ++n_reduce;  // consumed by test_reduce.cpp
      continue;
    }
    if (function != "age_equation_pair") {
      ADD_FAILURE() << name << ": unhandled function " << function;
      continue;
    }
    ++n_pair;
    known_keys(c,
               {"name", "source", "inputs", "expected", "tol", "legacy_sentinel",
                "expect_diagnostics", "expect_error"},
               name + " case");
    known_keys(in, {"function", "constants", "j", "f_a", "f_b", "lambda_k_total"},
               name + " inputs");
    if (!c["legacy_sentinel"].is_null()) ADD_FAILURE() << name << ": unhandled legacy_sentinel";
    if (!c["expect_error"].is_null()) ADD_FAILURE() << name << ": unhandled expect_error";
    if (c["expect_diagnostics"].size() != 0u) {
      ADD_FAILURE() << name << ": unhandled expect_diagnostics";
    }
    const g::Tol t = g::tol_of(c);
    ReductionConstants rc;
    if (!constants_of(in["constants"], rc, name)) continue;
    std::optional<Measured> lk;
    if (!in["lambda_k_total"].is_null()) lk = measured_of(in["lambda_k_total"]);

    const Measured jm = measured_of(in["j"]);
    const UFloat j = UFloat::variable(jm.value, jm.error, "J");  // one J, shared
    const Measured fam = measured_of(in["f_a"]);
    const Measured fbm = measured_of(in["f_b"]);
    const UFloat fa = UFloat::variable(fam.value, fam.error, "F");
    const UFloat fb = UFloat::variable(fbm.value, fbm.error, "F");
    const auto age_a = age_equation(j, fa, rc, lk);
    const auto age_b = age_equation(j, fb, rc, lk);
    if (!age_a || !age_b) {
      ADD_FAILURE() << name << ": age error";
      continue;
    }
    const std::map<std::string, const UFloat*> vars{
        {"f_a", &fa}, {"f_b", &fb}, {"age_a", &*age_a}, {"age_b", &*age_b}};

    std::size_t n_cov = 0;
    for (const auto& [key, w] : c["expected"].as_object()) {
      if (key == "cov") {
        for (const g::Json& row : w.as_array()) {
          const auto x = vars.find(row[0].as_string());
          const auto y = vars.find(row[1].as_string());
          if (x == vars.end() || y == vars.end()) {
            ADD_FAILURE() << name << ": unknown cov name";
            continue;
          }
          ++n_cov;
          g::expect_close(covariance(*x->second, *y->second), row[2].as_number(), t.rtol_err,
                          t.atol_err, "cov(" + x->first + ", " + y->first + ")");
        }
        continue;
      }
      const auto it = vars.find(key);
      if (it == vars.end()) {
        ADD_FAILURE() << name << ": unhandled expected key " << key;
        continue;
      }
      g::expect_close(it->second->nominal(), w["v"].as_number(), t.rtol, t.atol, key + ".v");
      g::expect_close(it->second->std_dev(), w["e"].as_number(), t.rtol_err, t.atol_err,
                      key + ".e");
    }
    EXPECT_EQ(n_cov, 10u);

    // The cross-covariance comes from J alone (lambda_k is fresh per call):
    // cov = dA/dJ dB/dJ sigma_J^2.
    const VariableId jid = j.variable_id();
    const double dA = age_a->derivative(jid);
    const double dB = age_b->derivative(jid);
    const double F_a = fa.nominal(), F_b = fb.nominal(), J = jm.value;
    const double lambda = rc.lambda_b.value + rc.lambda_e.value;
    const double scale = age_scale(AgeUnits::a, rc.age_units);
    expect_rel(dA, F_a / (lambda * (1.0 + J * F_a)) * scale, 1e-12, "dA/dJ");
    expect_rel(dB, F_b / (lambda * (1.0 + J * F_b)) * scale, 1e-12, "dB/dJ");
    expect_rel(covariance(*age_a, *age_b), dA * dB * jm.error * jm.error, 1e-12, "cov via J");
  }
  EXPECT_EQ(n_pair, 2u);
  EXPECT_EQ(n_reduce, 3u);
}
