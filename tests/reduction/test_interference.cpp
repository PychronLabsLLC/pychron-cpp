// K/Ca interference corrections (spec 3.3, E9-E11; Review Focus 1 and 5).
#include <gtest/gtest.h>

#include <cstdio>
#include <map>
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

// The production ratios of legacy test_k38_proportional_to_k39 with the
// "ratio_errors" golden errors, so every ratio is an independent variable.
ProductionVariables ratios_with_errors() {
  ProductionRatios p;
  p.k3739 = {0.01, 0.0005};
  p.k3839 = {0.013, 0.0002};
  p.ca3937 = {0.0007, 2e-5};
  p.ca3837 = {0.00019, 3e-6};
  p.ca3637 = {0.00026, 4e-6};
  return make_production_variables(p);
}

std::vector<std::string> names_of(const std::vector<Diagnostic>& d) {
  std::vector<std::string> out;
  for (const Diagnostic x : d) out.emplace_back(to_string(x));
  return out;
}

// Same nominal and the same derivative for every variable of either side.
void expect_identical(const UFloat& got, const UFloat& want, const char* what) {
  EXPECT_EQ(got.nominal(), want.nominal()) << what;
  ASSERT_EQ(got.terms().size(), want.terms().size()) << what;
  for (const UFloat::Term& t : want.terms()) {
    EXPECT_EQ(got.derivative(t.id), t.deriv) << what << " id " << t.id;
  }
}

}  // namespace

TEST(Interference, DiagnosticNames) {
  EXPECT_EQ(to_string(Diagnostic::FUndefined), "FUndefined");
  EXPECT_EQ(to_string(Diagnostic::YieldUndefined), "YieldUndefined");
  EXPECT_EQ(to_string(Diagnostic::AgeUndefined), "AgeUndefined");
  EXPECT_EQ(to_string(Diagnostic::KCaUndefined), "KCaUndefined");
  EXPECT_EQ(to_string(Diagnostic::KClUndefined), "KClUndefined");
  EXPECT_EQ(to_string(Diagnostic::CaClampedToZero), "CaClampedToZero");
  EXPECT_EQ(to_string(Diagnostic::FixedK3739ZeroCa3937), "FixedK3739ZeroCa3937");
  EXPECT_EQ(to_string(Diagnostic::NonFiniteResult), "NonFiniteResult");
}

// legacy:processing/tests/argon_calculations_test.py:199-206
TEST(Interference, PureKNoCa) {
  const UFloat a39 = UFloat::variable(100.0, 1.0, "Ar39");
  const UFloat a37 = UFloat::variable(0.0, 0.0, "Ar37");
  ProductionRatios p;
  p.k3839 = {0.013, 0.0};
  const ProductionVariables pv = make_production_variables(p);
  InterferenceOptions o;
  o.allow_negative_ca_correction = true;  // legacy ArArConstants trait default
  std::vector<Diagnostic> diags;
  const InterferenceComponents r = interference_corrections(a39, a37, pv, o, &diags);
  EXPECT_DOUBLE_EQ(r.k39.nominal(), 100.0);
  EXPECT_EQ(r.k39.derivative(a39.variable_id()), 1.0);
  EXPECT_EQ(r.ca37.nominal(), 0.0);
  EXPECT_DOUBLE_EQ(r.k38.nominal(), 1.3);
  EXPECT_DOUBLE_EQ(r.k38.derivative(a39.variable_id()), 0.013);
  EXPECT_TRUE(diags.empty());
}

// Review Focus 1: Ar37 reaches k39 through the numerator and ca37 through a37
// and k37; Ca3937 reaches k39 through numerator and denominator. Each output
// carries one merged derivative per variable.
TEST(Interference, NormalModeDerivatives) {
  const UFloat a39 = UFloat::variable(100.0, 1.0, "Ar39");
  const UFloat a37 = UFloat::variable(5.0, 0.05, "Ar37");
  const ProductionVariables pv = ratios_with_errors();
  InterferenceOptions o;
  o.allow_negative_ca_correction = true;
  std::vector<Diagnostic> diags;
  const InterferenceComponents r = interference_corrections(a39, a37, pv, o, &diags);
  EXPECT_TRUE(diags.empty());

  const double c = pv.ca3937.nominal();
  const double k = pv.k3739.nominal();
  const double a39v = a39.nominal(), a37v = a37.nominal();
  const double d = 1.0 - k * c;
  const VariableId i39 = a39.variable_id(), i37 = a37.variable_id();
  const VariableId ic = pv.ca3937.variable_id(), ik = pv.k3739.variable_id();

  // dk39/da37 = -Ca3937 / (1 - K3739 Ca3937), evaluated as the kernel does:
  // (-c) from a39 - c a37, then times 1/D from the quotient rule.
  EXPECT_EQ(r.k39.derivative(i37), -c * (1.0 / d));
  EXPECT_DOUBLE_EQ(r.k39.derivative(i37), -c / d);
  EXPECT_EQ(r.k39.derivative(i39), 1.0 / d);
  const double k39v = (a39v - c * a37v) / d;
  EXPECT_DOUBLE_EQ(r.k39.nominal(), k39v);
  // Ca3937 in numerator and denominator: -a37/D + (a39 - c a37) k / D^2.
  EXPECT_DOUBLE_EQ(r.k39.derivative(ic), -a37v / d + (a39v - c * a37v) * k / (d * d));
  // K3739 only in the denominator: (a39 - c a37) c / D^2.
  EXPECT_DOUBLE_EQ(r.k39.derivative(ik), (a39v - c * a37v) * c / (d * d));
  EXPECT_EQ(r.k39.terms().size(), 4u);

  // ca37 = a37 - K3739 k39: dca37/da37 = 1 + K c / D = 1 / D (merged once).
  EXPECT_DOUBLE_EQ(r.ca37.derivative(i37), 1.0 / d);
  EXPECT_DOUBLE_EQ(r.ca37.derivative(i39), -k / d);
  EXPECT_DOUBLE_EQ(r.ca37.derivative(ik), -k39v - k * (a39v - c * a37v) * c / (d * d));
  EXPECT_EQ(r.ca37.terms().size(), 4u);
  // ca39 = Ca3937 ca37: Ca3937 enters directly and through ca37.
  const double ca37v = a37v - k * k39v;
  EXPECT_DOUBLE_EQ(r.ca39.nominal(), c * ca37v);
  EXPECT_DOUBLE_EQ(r.ca39.derivative(i37), c / d);
  EXPECT_DOUBLE_EQ(r.ca39.derivative(ic), ca37v + c * r.ca37.derivative(ic));
  // k37 = K3739 k39, k38 = K3839 k39, ca36/ca38 from ca37.
  EXPECT_DOUBLE_EQ(r.k37.derivative(i37), k * (-c / d));
  EXPECT_DOUBLE_EQ(r.k37.derivative(ik), k39v + k * r.k39.derivative(ik));
  EXPECT_DOUBLE_EQ(r.k38.derivative(pv.k3839.variable_id()), k39v);
  EXPECT_DOUBLE_EQ(r.ca36.derivative(i37), pv.ca3637.nominal() / d);
  EXPECT_DOUBLE_EQ(r.ca38.derivative(pv.ca3837.variable_id()), ca37v);
}

TEST(Interference, FixedModeByConstantsAndByAnalysis) {
  const UFloat a39 = UFloat::variable(100.0, 1.0, "Ar39");
  const UFloat a37 = UFloat::variable(1.0, 0.01, "Ar37");
  const ProductionVariables pv = ratios_with_errors();
  const double c = pv.ca3937.nominal();

  // Per-analysis value forces fixed mode while mode == Normal.
  InterferenceOptions o;
  o.mode = K3739Mode::Normal;
  o.fixed_k3739 = UFloat::variable(0.05, 0.001);
  o.constants_fixed_k3739 = UFloat::variable(0.01, 0.0001, "k3739");
  o.allow_negative_ca_correction = true;
  std::vector<Diagnostic> diags;
  InterferenceComponents r = interference_corrections(a39, a37, pv, o, &diags);
  EXPECT_TRUE(diags.empty());
  const UFloat& x = *o.fixed_k3739;
  const double xv = 0.05, yv = 1.0 / c;
  EXPECT_DOUBLE_EQ(r.ca37.nominal(), 100.0 * xv * yv / (xv + yv));
  expect_identical(r.k37, x * r.k39, "k37 == x k39");
  EXPECT_DOUBLE_EQ(r.k39.nominal(), 100.0 - c * r.ca37.nominal());
  // a37 has no influence on any output; K3739 and the constants value neither.
  for (const UFloat* v : {&r.k37, &r.k38, &r.k39, &r.ca36, &r.ca37, &r.ca38, &r.ca39}) {
    EXPECT_EQ(v->derivative(a37.variable_id()), 0.0);
    EXPECT_EQ(v->derivative(pv.k3739.variable_id()), 0.0);
    EXPECT_EQ(v->derivative(o.constants_fixed_k3739.variable_id()), 0.0);
  }
  // a39 reaches k39 directly and through ca39: 1 - c x y / (x + y).
  EXPECT_DOUBLE_EQ(r.k39.derivative(a39.variable_id()), 1.0 - c * xv * yv / (xv + yv));
  EXPECT_NE(r.k39.derivative(x.variable_id()), 0.0);

  // Fixed by constants: mode == Fixed, no per-analysis value.
  InterferenceOptions oc;
  oc.mode = K3739Mode::Fixed;
  oc.constants_fixed_k3739 = UFloat::variable(0.01, 0.0001, "k3739");
  oc.allow_negative_ca_correction = true;
  diags.clear();
  r = interference_corrections(a39, a37, pv, oc, &diags);
  EXPECT_TRUE(diags.empty());
  const double xc = 0.01;
  EXPECT_DOUBLE_EQ(r.ca37.nominal(), 100.0 * xc * yv / (xc + yv));
  expect_identical(r.k37, oc.constants_fixed_k3739 * r.k39, "k37 == x k39 (constants)");
  EXPECT_NE(r.k39.derivative(oc.constants_fixed_k3739.variable_id()), 0.0);
  for (const UFloat* v : {&r.k37, &r.k38, &r.k39, &r.ca36, &r.ca37, &r.ca38, &r.ca39}) {
    EXPECT_EQ(v->derivative(a37.variable_id()), 0.0);
  }

  // Normal mode ignores the constants value.
  InterferenceOptions on;
  on.constants_fixed_k3739 = oc.constants_fixed_k3739;
  on.allow_negative_ca_correction = true;
  r = interference_corrections(a39, a37, pv, on, nullptr);
  EXPECT_EQ(r.k39.derivative(on.constants_fixed_k3739.variable_id()), 0.0);
  EXPECT_NE(r.k39.derivative(a37.variable_id()), 0.0);
}

// Spec Q10 / E10: y = 1 when nom(Ca3937) == 0 (legacy ZeroDivisionError).
TEST(Interference, FixedModeZeroCa3937UsesYOne) {
  const UFloat a39 = UFloat::variable(100.0, 1.0, "Ar39");
  const UFloat a37 = UFloat::variable(1.0, 0.01, "Ar37");
  ProductionRatios p;
  p.k3839 = {0.013, 0.0};
  p.ca3937 = {0.0, 1e-5};  // zero nominal with an error still takes y = 1
  const ProductionVariables pv = make_production_variables(p);
  InterferenceOptions o;
  o.fixed_k3739 = UFloat::variable(0.05, 0.001);
  o.allow_negative_ca_correction = true;
  std::vector<Diagnostic> diags;
  const InterferenceComponents r = interference_corrections(a39, a37, pv, o, &diags);
  const double x = 0.05;
  EXPECT_DOUBLE_EQ(r.ca37.nominal(), 100.0 * x / (x + 1.0));
  EXPECT_DOUBLE_EQ(r.ca37.derivative(a39.variable_id()), x / (x + 1.0));
  EXPECT_DOUBLE_EQ(r.ca37.derivative(o.fixed_k3739->variable_id()),
                   100.0 / ((x + 1.0) * (x + 1.0)));
  EXPECT_EQ(r.ca39.nominal(), 0.0);
  EXPECT_DOUBLE_EQ(r.ca39.derivative(pv.ca3937.variable_id()), r.ca37.nominal());
  EXPECT_EQ(names_of(diags), std::vector<std::string>{"FixedK3739ZeroCa3937"});

  // Missing Ca3937 (exact 0) under constants fixed mode: same fallback.
  ProductionRatios pm;
  const ProductionVariables pvm = make_production_variables(pm);
  InterferenceOptions oc;
  oc.mode = K3739Mode::Fixed;
  oc.constants_fixed_k3739 = UFloat::variable(0.01, 0.0001, "k3739");
  oc.allow_negative_ca_correction = true;
  diags.clear();
  const InterferenceComponents m = interference_corrections(a39, a37, pvm, oc, &diags);
  EXPECT_DOUBLE_EQ(m.ca37.nominal(), 100.0 * 0.01 / 1.01);
  EXPECT_TRUE(m.ca39.is_exact());
  EXPECT_EQ(m.ca39.nominal(), 0.0);
  EXPECT_EQ(names_of(diags), std::vector<std::string>{"FixedK3739ZeroCa3937"});

  // Normal mode with Ca3937 == 0 is not the fallback: no diagnostic.
  InterferenceOptions on;
  on.allow_negative_ca_correction = true;
  diags.clear();
  (void)interference_corrections(a39, a37, pvm, on, &diags);
  EXPECT_TRUE(diags.empty());
}

// Review Focus 5 / spec Q8: the clamp runs after ca39 and k39 used the
// unclamped ca37; the clamped ca37 is exact zero with no terms.
TEST(Interference, ClampAfterCa39) {
  const UFloat a39 = UFloat::variable(100.0, 1.0, "Ar39");
  const UFloat a37 = UFloat::variable(-0.5, 0.01, "Ar37");
  const ProductionVariables pv = ratios_with_errors();

  InterferenceOptions off;
  off.allow_negative_ca_correction = true;
  std::vector<Diagnostic> d_off;
  const InterferenceComponents keep = interference_corrections(a39, a37, pv, off, &d_off);
  EXPECT_TRUE(d_off.empty());
  ASSERT_LT(keep.ca37.nominal(), 0.0);
  EXPECT_LT(keep.ca36.nominal(), 0.0);
  EXPECT_LT(keep.ca38.nominal(), 0.0);
  EXPECT_LT(keep.ca39.nominal(), 0.0);
  EXPECT_FALSE(keep.ca37.is_exact());

  InterferenceOptions on;
  on.allow_negative_ca_correction = false;
  std::vector<Diagnostic> d_on;
  const InterferenceComponents cl = interference_corrections(a39, a37, pv, on, &d_on);
  EXPECT_EQ(names_of(d_on), std::vector<std::string>{"CaClampedToZero"});
  EXPECT_EQ(cl.ca37.nominal(), 0.0);
  EXPECT_TRUE(cl.ca37.is_exact());
  EXPECT_EQ(cl.ca36.nominal(), 0.0);
  EXPECT_TRUE(cl.ca36.is_exact());
  EXPECT_EQ(cl.ca38.nominal(), 0.0);
  EXPECT_TRUE(cl.ca38.is_exact());
  // ca39, k39, k37, k38 still carry the negative, unclamped ca37.
  expect_identical(cl.ca39, keep.ca39, "ca39");
  expect_identical(cl.k39, keep.k39, "k39");
  expect_identical(cl.k37, keep.k37, "k37");
  expect_identical(cl.k38, keep.k38, "k38");
  EXPECT_DOUBLE_EQ(cl.ca39.nominal(), pv.ca3937.nominal() * keep.ca37.nominal());
  EXPECT_NE(cl.ca39.derivative(a37.variable_id()), 0.0);
  EXPECT_NE(cl.k39.derivative(a37.variable_id()), 0.0);

  // Positive ca37 is not clamped, and exactly zero is (Python max keeps the
  // first argument, ufloat(0, 0), unless ca37 > 0).
  const UFloat pos = UFloat::variable(5.0, 0.05, "Ar37");
  d_on.clear();
  const InterferenceComponents kept = interference_corrections(a39, pos, pv, on, &d_on);
  EXPECT_TRUE(d_on.empty());
  EXPECT_GT(kept.ca37.nominal(), 0.0);
  EXPECT_FALSE(kept.ca37.is_exact());
}

TEST(Interference, NullDiagnosticsPointerIsAllowed) {
  const UFloat a39 = UFloat::variable(100.0, 1.0, "Ar39");
  const UFloat a37 = UFloat::variable(-0.5, 0.01, "Ar37");
  const InterferenceComponents r =
      interference_corrections(a39, a37, ratios_with_errors(), InterferenceOptions{});
  EXPECT_TRUE(r.ca37.is_exact());
}

TEST(Interference, Golden) {
  const g::Json doc = g::load("interference.json");
  const g::Json& cases = doc["cases"];
  ASSERT_EQ(cases.size(), 26u);
  std::size_t n_legacy = 0, n_prefs = 0, n_fixed = 0, n_diag = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const g::Json& c = cases[i];
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    if (!c["legacy_sentinel"].is_null()) ADD_FAILURE() << name << ": unhandled legacy_sentinel";
    if (!c["expect_error"].is_null()) ADD_FAILURE() << name << ": unhandled expect_error";
    if (name.ends_with("@legacy")) ++n_legacy;
    else if (name.ends_with("@legacy_preferences")) ++n_prefs;
    const g::Tol t = g::tol_of(c);
    const g::Json& in = c["inputs"];
    const g::Json& k = in["constants"];

    std::map<std::string, Measured, std::less<>> rows;
    for (const auto& [key, m] : in["production"].as_object()) {
      rows[key] = {m["v"].as_number(), m["e"].as_number()};
    }
    auto pr = production_from_rows(rows);
    if (!pr) {
      ADD_FAILURE() << name << ": " << pr.error().what;
      continue;
    }
    const ProductionVariables pv = make_production_variables(*pr);

    InterferenceOptions o;
    const std::string& mode = k["k3739_mode"].as_string();
    if (mode == "Fixed") {
      o.mode = K3739Mode::Fixed;
    } else if (mode != "Normal") {
      ADD_FAILURE() << name << ": unknown k3739_mode " << mode;
      continue;
    }
    o.constants_fixed_k3739 = UFloat::variable(k["fixed_k3739"]["v"].as_number(),
                                               k["fixed_k3739"]["e"].as_number(), "k3739");
    if (!in["fixed_k3739"].is_null()) {
      o.fixed_k3739 = UFloat::variable(in["fixed_k3739"]["v"].as_number(),
                                       in["fixed_k3739"]["e"].as_number());
    }
    if (o.mode == K3739Mode::Fixed || o.fixed_k3739) ++n_fixed;
    o.allow_negative_ca_correction = k["allow_negative_ca_correction"].as_bool();

    const UFloat a39 =
        UFloat::variable(in["a39"]["v"].as_number(), in["a39"]["e"].as_number(), "Ar39");
    const UFloat a37 =
        UFloat::variable(in["a37"]["v"].as_number(), in["a37"]["e"].as_number(), "Ar37");
    std::vector<Diagnostic> diags;
    const InterferenceComponents r = interference_corrections(a39, a37, pv, o, &diags);

    std::vector<std::string> want_diags;
    for (const g::Json& d : c["expect_diagnostics"].as_array()) want_diags.push_back(d.as_string());
    if (!want_diags.empty()) ++n_diag;
    EXPECT_EQ(names_of(diags), want_diags);

    const std::map<std::string, const UFloat*> got{
        {"k37", &r.k37},   {"k38", &r.k38},   {"k39", &r.k39},  {"ca36", &r.ca36},
        {"ca37", &r.ca37}, {"ca38", &r.ca38}, {"ca39", &r.ca39}};
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
  EXPECT_EQ(n_legacy, 13u);
  EXPECT_EQ(n_prefs, 13u);
  EXPECT_EQ(n_fixed, 12u);
  EXPECT_EQ(n_diag, 7u);
}
