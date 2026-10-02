// Atmospheric, chlorine and cosmogenic components (spec 3.4, E12-E13; Q1,
// Q16; Review Focus 1).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <string>
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

// Legacy ArArConstants trait values for the constants E12 reads
// (arar_constants.py:28-94), with a non-zero lambda_Cl36 error so that it is
// a variable too.
ReductionConstants legacy_constants() {
  ReductionConstants c = constants_preset(ConstantsPreset::Legacy);
  c.lambda_cl36 = {6.308e-9, 1e-11};
  return c;
}

std::vector<VariableId> ids_tagged(const UFloat& x, std::string_view tag) {
  std::vector<VariableId> out;
  for (const UFloat::Term& t : x.terms()) {
    if (tag_name(t.tag) == tag) out.push_back(t.id);
  }
  return out;
}

void expect_rel(double got, double want, double rtol, const std::string& what) {
  EXPECT_LE(std::fabs(got - want), rtol * std::fabs(want))
      << what << ": got " << ::testing::PrintToString(got) << ", want "
      << ::testing::PrintToString(want);
}

// Cl3638 with 1 - ((Cl3638 * lCl) * dd) * (atm4036 / atm4038) == 0 exactly,
// as the golden generator searches for it (find_singular_cl3638).
double singular_cl3638(const ReductionConstants& c, double dd) {
  const double lcl = c.lambda_cl36.value;
  const double r = c.atm4036.value / c.atm4038.value;
  double cl = 1.0 / (lcl * dd * r);
  for (int i = 0; i < 64; ++i) {
    const double x = ((cl * lcl) * dd) * r;
    if (1.0 - x == 0.0) return cl;
    cl = std::nextafter(cl, x < 1.0 ? std::numeric_limits<double>::infinity()
                                    : -std::numeric_limits<double>::infinity());
  }
  return std::numeric_limits<double>::quiet_NaN();
}

Measured measured_of(const g::Json& j) { return {j["v"].as_number(), j["e"].as_number()}; }

}  // namespace

// legacy:processing/tests/argon_calculations_test.py AtmosphericTest
// test_no_chlorine_atm36_is_nonradiogenic
TEST(Atmospheric, NoChlorineIsSimpleSubtraction) {
  const UFloat a38 = UFloat::variable(0.5, 0.005, "Ar38");
  const UFloat a36 = UFloat::variable(0.1, 0.001, "Ar36");
  const UFloat k38 = UFloat::variable(0.013, 0.0001);
  const UFloat ca38 = 0.00019;
  const UFloat ca36 = UFloat::variable(0.00026, 1e-6);
  const auto r = atmospheric_components(a38, a36, k38, ca38, ca36, 365.0, UFloat(0.0),
                                        legacy_constants());
  ASSERT_TRUE(r) << r.error().what;
  const UFloat want = a36 - ca36;
  EXPECT_EQ(r->atm36.nominal(), want.nominal());
  EXPECT_EQ(r->atm36.terms().size(), 2u);
  EXPECT_EQ(r->atm36.derivative(a36.variable_id()), 1.0);
  EXPECT_EQ(r->atm36.derivative(ca36.variable_id()), -1.0);
  EXPECT_DOUBLE_EQ(r->atm36.std_dev(), want.std_dev());
  EXPECT_EQ(r->cl36.nominal(), 0.0);
  EXPECT_TRUE(r->cl36.is_exact());
  // cl38 = a38 - atm38 - k38 - ca38 still carries the atmospheric 38.
  EXPECT_DOUBLE_EQ(r->cl38.nominal(), 0.5 - r->atm38.nominal() - 0.013 - 0.00019);
  EXPECT_EQ(r->cl38.derivative(a38.variable_id()), 1.0);
  EXPECT_EQ(r->cl38.derivative(k38.variable_id()), -1.0);
}

// legacy test_atm38_proportional_to_atm36 / test_atm3836_uncertainty_propagates_to_atm38
TEST(Atmospheric, Atm38FollowsRatioWithError) {
  const UFloat a38 = 0.5;
  const UFloat a36 = UFloat::variable(0.1, 0.001, "Ar36");
  const UFloat k38 = 0.013;
  const ReductionConstants c = legacy_constants();
  const auto r = atmospheric_components(a38, a36, k38, UFloat(0.0), UFloat(0.0), 365.0,
                                        UFloat(0.0), c);
  ASSERT_TRUE(r) << r.error().what;
  const double r3836 = c.atm4036.value / c.atm4038.value;
  EXPECT_DOUBLE_EQ(r->atm38.nominal(), r3836 * r->atm36.nominal());

  // Legacy parity (argon_calculations.py:470-479): one variable tagged
  // "atm3836", no atm4036 / atm4038 terms.
  EXPECT_TRUE(ids_tagged(r->atm38, "atm4036").empty());
  EXPECT_TRUE(ids_tagged(r->atm38, "atm4038").empty());
  ASSERT_EQ(r->atm38.terms().size(), 2u);  // Ar36 and atm3836
  const auto i3836 = ids_tagged(r->atm38, "atm3836");
  ASSERT_EQ(i3836.size(), 1u);
  // Its sigma is the quadrature of the atm4036 and atm4038 errors.
  const double a = c.atm4036.value, b = c.atm4038.value;
  const double sigma = std::sqrt(std::pow(c.atm4036.error / b, 2) +
                                 std::pow(a * c.atm4038.error / (b * b), 2));
  const UFloat::Term& t = r->atm38.terms()[r->atm38.terms()[0].id == i3836[0] ? 0 : 1];
  ASSERT_EQ(t.id, i3836[0]);
  expect_rel(t.sigma, sigma, 1e-14, "atm3836 sigma");
  // d atm38 / d atm3836 = atm36.
  const double atm36 = r->atm36.nominal();
  EXPECT_EQ(r->atm38.derivative(i3836[0]), atm36);
  EXPECT_EQ(r->atm38.derivative(a36.variable_id()), r3836);
  EXPECT_GT(r->atm38.std_dev(), r3836 * r->atm36.std_dev());
  // Without chlorine atm36 does not depend on the ratio (m == 0 exactly).
  EXPECT_TRUE(ids_tagged(r->atm36, "atm3836").empty());
  // cl38 = a38 - atm38 - k38 - ca38 carries the ratio with opposite sign.
  EXPECT_EQ(r->cl38.derivative(i3836[0]), -atm36);
}

// Spec Q1 / Review Focus 1: every call mints its own lambda_Cl36 and atm3836,
// so two analyses are independent in their constants (D6). The E12 path has
// no atm4036-tagged term, so it cannot share a variable with E14's
// trapped_4036 (the other legacy atm4036 copy).
TEST(Atmospheric, FreshConstantVariablesPerCall) {
  const UFloat a38 = UFloat::variable(0.5, 0.005, "Ar38");
  const UFloat a36 = UFloat::variable(0.1, 0.001, "Ar36");
  const UFloat k38 = UFloat::variable(0.013, 0.0001);
  const UFloat cl3638 = UFloat::variable(250.0, 5.0, "Cl3638");
  const ReductionConstants c = legacy_constants();
  const auto r1 = atmospheric_components(a38, a36, k38, UFloat(0.0), UFloat(0.0), 365.0,
                                         cl3638, c);
  const auto r2 = atmospheric_components(a38, a36, k38, UFloat(0.0), UFloat(0.0), 365.0,
                                         cl3638, c);
  ASSERT_TRUE(r1 && r2);
  for (const UFloat* x : {&r1->atm36, &r1->atm38, &r1->cl36, &r1->cl38}) {
    EXPECT_TRUE(ids_tagged(*x, "atm4036").empty());
    EXPECT_TRUE(ids_tagged(*x, "atm4038").empty());
    EXPECT_TRUE(ids_tagged(*x, "trapped_4036").empty());
  }
  for (const char* tag : {"atm3836", "lambda_Cl36"}) {
    const auto ids1 = ids_tagged(r1->atm36, tag);
    const auto ids2 = ids_tagged(r2->atm36, tag);
    ASSERT_EQ(ids1.size(), 1u) << tag;
    ASSERT_EQ(ids2.size(), 1u) << tag;
    EXPECT_NE(ids1[0], ids2[0]) << tag;
    // Within one call the same variable feeds every output.
    EXPECT_EQ(ids_tagged(r1->atm38, tag), ids1) << tag;
    EXPECT_EQ(ids_tagged(r1->cl36, tag), ids1) << tag;
    EXPECT_EQ(ids_tagged(r1->cl38, tag), ids1) << tag;
    // The other call's constant does not reach this call's outputs.
    EXPECT_EQ(r1->atm36.derivative(ids2[0]), 0.0) << tag;
  }
  // Shared signal variables are shared, so the two results are correlated
  // through them only: same nominal, same signal derivatives.
  EXPECT_EQ(r1->atm36.nominal(), r2->atm36.nominal());
  EXPECT_EQ(r1->atm36.derivative(a36.variable_id()), r2->atm36.derivative(a36.variable_id()));
  EXPECT_EQ(r1->atm36.derivative(cl3638.variable_id()),
            r2->atm36.derivative(cl3638.variable_id()));
}

// legacy CalculateAtmosphericChlorineTest test_with_chlorine_yields_nonzero_cl
TEST(Atmospheric, ChlorineBranch) {
  const UFloat a38 = UFloat::variable(0.5, 0.005, "Ar38");
  const UFloat a36 = UFloat::variable(0.1, 0.001, "Ar36");
  const UFloat k38 = UFloat::variable(0.013, 0.0001);
  const UFloat zero = 0.0;
  const UFloat cl3638 = UFloat::variable(250.0, 5.0, "Cl3638");
  const ReductionConstants c = legacy_constants();
  const double dd = 365.0;
  const auto r = atmospheric_components(a38, a36, k38, zero, zero, dd, cl3638, c);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_NE(r->cl36.nominal(), 0.0);
  EXPECT_GT(r->atm36.std_dev(), 0.0);

  const double l = c.lambda_cl36.value;
  const double rr = c.atm4036.value / c.atm4038.value;
  const double m = 250.0 * l * dd;
  const double A = 0.1, B = 0.5 - 0.013;
  const double D = 1.0 - m * rr;
  const double atm36 = (A - m * B) / D;
  expect_rel(r->atm36.nominal(), atm36, 1e-14, "atm36");
  expect_rel(r->cl36.nominal(), m * (B - rr * atm36), 1e-14, "cl36");

  // E12 partials with respect to Cl3638: datm36/dm = (r atm36 - B) / D,
  // dm/dCl3638 = lCl dd.
  const VariableId ic = cl3638.variable_id();
  const double datm36 = l * dd * (rr * atm36 - B) / D;
  expect_rel(r->atm36.derivative(ic), datm36, 1e-12, "datm36/dCl3638");
  expect_rel(r->atm38.derivative(ic), rr * datm36, 1e-12, "datm38/dCl3638");
  expect_rel(r->cl38.derivative(ic), -rr * datm36, 1e-12, "dcl38/dCl3638");
  const double cl38 = B - rr * atm36;
  expect_rel(r->cl36.derivative(ic), -rr * datm36 * m + cl38 * l * dd, 1e-12,
             "dcl36/dCl3638");
  // Signal partials: datm36/da36 = 1/D, datm36/da38 = -m/D.
  expect_rel(r->atm36.derivative(a36.variable_id()), 1.0 / D, 1e-14, "datm36/da36");
  expect_rel(r->atm36.derivative(a38.variable_id()), -m / D, 1e-14, "datm36/da38");
  expect_rel(r->atm36.derivative(k38.variable_id()), m / D, 1e-14, "datm36/dk38");
  // lambda_Cl36 enters through m exactly as Cl3638 does, scaled.
  const auto il = ids_tagged(r->atm36, "lambda_Cl36");
  ASSERT_EQ(il.size(), 1u);
  expect_rel(r->atm36.derivative(il[0]), 250.0 * dd * (rr * atm36 - B) / D, 1e-12,
             "datm36/dlambda_Cl36");
  // The ratio enters atm36 through the denominator: datm36/dr = m atm36 / D;
  // atm38 = r atm36 adds atm36 directly.
  const auto i3836 = ids_tagged(r->atm36, "atm3836");
  ASSERT_EQ(i3836.size(), 1u);
  expect_rel(r->atm36.derivative(i3836[0]), m * atm36 / D, 1e-12, "datm36/datm3836");
  expect_rel(r->atm38.derivative(i3836[0]), atm36 + rr * m * atm36 / D, 1e-12,
             "datm38/datm3836");
}

// Spec Q16: legacy ZeroDivisionError becomes an error Result.
TEST(Atmospheric, SingularDenominatorErrors) {
  const ReductionConstants c = legacy_constants();
  const double dd = 365.0;
  const double cl = singular_cl3638(c, dd);
  ASSERT_TRUE(std::isfinite(cl));
  const UFloat a38 = UFloat::variable(0.5, 0.005, "Ar38");
  const UFloat a36 = UFloat::variable(0.1, 0.001, "Ar36");
  const UFloat zero = 0.0;
  const auto r = atmospheric_components(a38, a36, zero, zero, zero, dd, UFloat(cl), c);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
  EXPECT_TRUE(r.error().what.starts_with("reduction: ")) << r.error().what;
  EXPECT_NE(r.error().what.find("zero divisor"), std::string::npos) << r.error().what;

  // Near but not exactly singular divides: no epsilon threshold (spec 7).
  const auto near =
      atmospheric_components(a38, a36, zero, zero, zero, dd, UFloat(cl * (1.0 - 1e-9)), c);
  ASSERT_TRUE(near) << near.error().what;
  EXPECT_TRUE(std::isfinite(near->atm36.nominal()));
  EXPECT_GT(std::fabs(near->atm36.nominal()), 1e6);
}

TEST(Atmospheric, InvalidConstantsError) {
  ReductionConstants c = legacy_constants();
  c.atm4038.error = -1.0;
  const auto r = atmospheric_components(UFloat(0.5), UFloat(0.1), UFloat(0.0), UFloat(0.0),
                                        UFloat(0.0), 365.0, UFloat(0.0), c);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
  EXPECT_NE(r.error().what.find("atm4038"), std::string::npos) << r.error().what;
}

// legacy CosmogenicComponentsTest (argon_calculations_test.py)
TEST(Cosmogenic, PureSolarPureCosmoMixed) {
  const CosmogenicRatios ratios{{0.1869, 0.001}, {0.65, 0.01}};

  const auto solar = cosmogenic_components(UFloat(1.0), UFloat(0.1869), ratios);
  ASSERT_TRUE(solar) << solar.error().what;
  EXPECT_NEAR(solar->cosmo38.nominal(), 0.0, 1e-12);
  EXPECT_NEAR(solar->cosmo36.nominal(), 0.0, 1e-12);
  EXPECT_NEAR(solar->noncosmo36.nominal(), 1.0, 1e-12);

  const auto cosmo = cosmogenic_components(UFloat(1.0), UFloat(0.65), ratios);
  ASSERT_TRUE(cosmo) << cosmo.error().what;
  EXPECT_NEAR(cosmo->noncosmo38.nominal(), 0.0, 1e-12);
  EXPECT_NEAR(cosmo->noncosmo36.nominal(), 0.0, 1e-12);
  EXPECT_NEAR(cosmo->cosmo36.nominal(), 1.0, 1e-12);

  const UFloat c36 = UFloat::variable(1.0, 0.01, "Ar36");
  const UFloat c38 = UFloat::variable(0.3, 0.003, "Ar38");
  const auto mixed = cosmogenic_components(c36, c38, ratios);
  ASSERT_TRUE(mixed) << mixed.error().what;
  EXPECT_NEAR(mixed->cosmo38.nominal() + mixed->noncosmo38.nominal(), 0.3, 1e-12);
  EXPECT_NEAR(mixed->cosmo36.nominal() + mixed->noncosmo36.nominal(), 1.0, 1e-12);

  // Derivatives (Review Focus 1): rm = c38/c36, fs = (rc - rm)/(rc - rs).
  const double rs = 0.1869, rc = 0.65, rm = 0.3;
  const double fs = (rc - rm) / (rc - rs);
  const VariableId i36 = c36.variable_id(), i38 = c38.variable_id();
  // dfs/dc38 = -1/(c36 (rc - rs)); dfs/dc36 = rm/(c36 (rc - rs)).
  const double dfs38 = -1.0 / (rc - rs), dfs36 = rm / (rc - rs);
  // noncosmo38 = fs c38: c38 reaches it twice (through fs and directly).
  expect_rel(mixed->noncosmo38.derivative(i38), fs + 0.3 * dfs38, 1e-12, "dnc38/dc38");
  expect_rel(mixed->noncosmo38.derivative(i36), 0.3 * dfs36, 1e-12, "dnc38/dc36");
  // cosmo36 = (1 - fs) c36.
  expect_rel(mixed->cosmo36.derivative(i36), (1.0 - fs) - dfs36, 1e-12, "dc36/dc36");
  expect_rel(mixed->cosmo36.derivative(i38), -dfs38, 1e-12, "dc36/dc38");
  // Components sum to the inputs: derivatives cancel exactly in the sum.
  const UFloat sum38 = mixed->cosmo38 + mixed->noncosmo38;
  EXPECT_NEAR(sum38.derivative(i38), 1.0, 1e-15);
  EXPECT_NEAR(sum38.derivative(i36), 0.0, 1e-15);
  // One fresh variable per ratio (tags solar3836, cosmo3836); rc appears in
  // both numerator and denominator of fs as one merged derivative.
  const auto is = ids_tagged(mixed->cosmo36, "solar3836");
  const auto ic = ids_tagged(mixed->cosmo36, "cosmo3836");
  ASSERT_EQ(is.size(), 1u);
  ASSERT_EQ(ic.size(), 1u);
  // dfs/drs = fs/(rc - rs); dfs/drc = (1 - fs)/(rc - rs); cosmo36 = (1 - fs) c36.
  expect_rel(mixed->cosmo36.derivative(is[0]), -fs / (rc - rs), 1e-12, "dc36/drs");
  expect_rel(mixed->cosmo36.derivative(ic[0]), -(1.0 - fs) / (rc - rs), 1e-12, "dc36/drc");
  const auto again = cosmogenic_components(c36, c38, ratios);
  ASSERT_TRUE(again);
  EXPECT_NE(ids_tagged(again->cosmo36, "solar3836"), is);

  // Spec Q16: c36 == 0 and rc == rs are errors (legacy ZeroDivisionError).
  const auto zero36 = cosmogenic_components(UFloat::variable(0.0, 0.01), UFloat(0.3), ratios);
  ASSERT_FALSE(zero36);
  EXPECT_EQ(zero36.error().kind, pychron::ErrorKind::Config);
  EXPECT_NE(zero36.error().what.find("zero divisor"), std::string::npos) << zero36.error().what;
  EXPECT_TRUE(zero36.error().what.starts_with("reduction: "));
  const auto same = cosmogenic_components(UFloat(1.0), UFloat(0.3),
                                          CosmogenicRatios{{0.4, 0.001}, {0.4, 0.01}});
  ASSERT_FALSE(same);
  EXPECT_NE(same.error().what.find("zero divisor"), std::string::npos) << same.error().what;
}

TEST(Atmospheric, Golden) {
  const g::Json doc = g::load("atmospheric.json");
  const g::Json& cases = doc["cases"];
  std::size_t n_ok = 0, n_err = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const g::Json& c = cases[i];
    const g::Json& in = c["inputs"];
    if (in["function"].as_string() != "atmospheric_components") continue;
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    if (!c["legacy_sentinel"].is_null()) ADD_FAILURE() << name << ": unhandled legacy_sentinel";
    if (!c["expect_diagnostics"].as_array().empty()) {
      ADD_FAILURE() << name << ": unhandled expect_diagnostics";
    }
    const g::Tol t = g::tol_of(c);
    const g::Json& k = in["constants"];
    ReductionConstants rc;
    rc.lambda_cl36 = measured_of(k["lambda_cl36"]);
    rc.atm4036 = measured_of(k["atm4036"]);
    rc.atm4038 = measured_of(k["atm4038"]);

    std::map<std::string, Measured, std::less<>> rows;
    for (const auto& [key, m] : in["production"].as_object()) rows[key] = measured_of(m);
    auto pr = production_from_rows(rows);
    if (!pr) {
      ADD_FAILURE() << name << ": " << pr.error().what;
      continue;
    }
    const ProductionVariables pv = make_production_variables(*pr);
    auto var = [&in](const char* key, const char* tag) {
      return UFloat::variable(in[key]["v"].as_number(), in[key]["e"].as_number(), tag);
    };
    const auto r = atmospheric_components(var("a38", "Ar38"), var("a36", "Ar36"),
                                          var("k38", "k38"), var("ca38", "ca38"),
                                          var("ca36", "ca36"), in["decay_days"].as_number(),
                                          pv.cl3638, rc);
    if (c["expect_error"].is_string()) {
      ++n_err;
      if (r) {
        ADD_FAILURE() << name << ": expected error containing " << c["expect_error"].string;
      } else {
        EXPECT_NE(r.error().what.find(c["expect_error"].string), std::string::npos)
            << r.error().what;
        EXPECT_TRUE(r.error().what.starts_with("reduction: ")) << r.error().what;
      }
      continue;
    }
    if (!c["expect_error"].is_null()) ADD_FAILURE() << name << ": unhandled expect_error";
    if (!r) {
      ADD_FAILURE() << name << ": " << r.error().what;
      continue;
    }
    ++n_ok;
    const std::map<std::string, const UFloat*> got{
        {"atm36", &r->atm36}, {"atm38", &r->atm38}, {"cl36", &r->cl36}, {"cl38", &r->cl38}};
    const g::Json& want = c["expected"];
    if (want.size() != got.size()) {
      ADD_FAILURE() << fmt("expected key count ", static_cast<double>(want.size()));
    }
    for (const auto& [key, w] : want.as_object()) {
      const auto it = got.find(key);
      if (it == got.end()) {
        ADD_FAILURE() << name << ": unhandled expected key " << key;
        continue;
      }
      g::expect_close(it->second->nominal(), w["v"].as_number(), t.rtol, t.atol, key + ".v");
      g::expect_close(it->second->std_dev(), w["e"].as_number(), t.rtol_err, t.atol_err,
                      key + ".e");
    }
  }
  EXPECT_EQ(n_ok, 6u);
  EXPECT_EQ(n_err, 1u);
}

TEST(Cosmogenic, Golden) {
  const g::Json doc = g::load("atmospheric.json");
  const g::Json& cases = doc["cases"];
  std::size_t n_ok = 0, n_err = 0, n_other = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const g::Json& c = cases[i];
    const g::Json& in = c["inputs"];
    const std::string& fn = in["function"].as_string();
    if (fn == "atmospheric_components") continue;
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    if (fn != "cosmogenic_components") {
      ++n_other;
      ADD_FAILURE() << name << ": unhandled function " << fn;
      continue;
    }
    if (!c["legacy_sentinel"].is_null()) ADD_FAILURE() << name << ": unhandled legacy_sentinel";
    if (!c["expect_diagnostics"].as_array().empty()) {
      ADD_FAILURE() << name << ": unhandled expect_diagnostics";
    }
    const g::Tol t = g::tol_of(c);
    const g::Json& cosmo = in["constants"]["cosmogenic"];
    if (!cosmo.is_object()) {
      ADD_FAILURE() << name << ": cosmogenic constants missing";
      continue;
    }
    const CosmogenicRatios ratios{measured_of(cosmo["solar3836"]),
                                  measured_of(cosmo["cosmo3836"])};
    const UFloat c36 = UFloat::variable(in["c36"]["v"].as_number(), in["c36"]["e"].as_number());
    const UFloat c38 = UFloat::variable(in["c38"]["v"].as_number(), in["c38"]["e"].as_number());
    const auto r = cosmogenic_components(c36, c38, ratios);
    if (c["expect_error"].is_string()) {
      ++n_err;
      if (r) {
        ADD_FAILURE() << name << ": expected error containing " << c["expect_error"].string;
      } else {
        EXPECT_NE(r.error().what.find(c["expect_error"].string), std::string::npos)
            << r.error().what;
      }
      continue;
    }
    if (!c["expect_error"].is_null()) ADD_FAILURE() << name << ": unhandled expect_error";
    if (!r) {
      ADD_FAILURE() << name << ": " << r.error().what;
      continue;
    }
    ++n_ok;
    const std::map<std::string, const UFloat*> got{{"cosmo36", &r->cosmo36},
                                                   {"cosmo38", &r->cosmo38},
                                                   {"noncosmo36", &r->noncosmo36},
                                                   {"noncosmo38", &r->noncosmo38}};
    const g::Json& want = c["expected"];
    if (want.size() != got.size()) {
      ADD_FAILURE() << fmt("expected key count ", static_cast<double>(want.size()));
    }
    for (const auto& [key, w] : want.as_object()) {
      const auto it = got.find(key);
      if (it == got.end()) {
        ADD_FAILURE() << name << ": unhandled expected key " << key;
        continue;
      }
      g::expect_close(it->second->nominal(), w["v"].as_number(), t.rtol, t.atol, key + ".v");
      g::expect_close(it->second->std_dev(), w["e"].as_number(), t.rtol_err, t.atol_err,
                      key + ".e");
    }
  }
  EXPECT_EQ(n_ok, 4u);
  EXPECT_EQ(n_err, 2u);
  EXPECT_EQ(n_other, 0u);
}
