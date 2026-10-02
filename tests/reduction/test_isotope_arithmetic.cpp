// Isotope arithmetic E1-E5 (spec 3.1, D4) and the isotope_arithmetic.json
// golden vectors.
#include "pychron/reduction/arar_reduction.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <string>

#include "golden.hpp"

namespace golden = pychron::reduction::golden;
using namespace pychron::reduction;

namespace {

UFloat var(double v, double e) { return UFloat::variable(v, e); }

UFloat ufloat_of(const golden::Json& j, std::string_view tag = {}) {
  return UFloat::variable(j["v"].as_number(), j["e"].as_number(), tag);
}

void expect_ufloat(const UFloat& got, const golden::Json& want, const golden::Tol& t,
                   const std::string& what) {
  golden::expect_close(got.nominal(), want["v"].as_number(), t.rtol, t.atol, what + " v");
  golden::expect_close(got.std_dev(), want["e"].as_number(), t.rtol_err, t.atol_err,
                       what + " e");
}

// |got - want| > atol + rtol |want| for the value or the error.
bool differs(const UFloat& got, const golden::Json& want, const golden::Tol& t) {
  const double wv = want["v"].as_number();
  const double we = want["e"].as_number();
  return std::fabs(got.nominal() - wv) > t.atol + t.rtol * std::fabs(wv) ||
         std::fabs(got.std_dev() - we) > t.atol_err + t.rtol_err * std::fabs(we);
}

const golden::Json& find_case(const golden::Json& doc, std::string_view name) {
  for (const golden::Json& c : doc["cases"].as_array()) {
    if (c["name"].as_string() == name) return c;
  }
  ADD_FAILURE() << "golden case not found: " << name;
  return golden::Json::null_value();
}

IsotopeSignal plain_signal() {
  IsotopeSignal s;
  s.intercept = var(100.0, 0.5);
  s.baseline = var(5.0, 0.2);
  s.blank = var(3.0, 0.1);
  s.discrimination = var(1.003, 0.001);
  s.ic_factor = var(1.01, 0.002);
  return s;
}

}  // namespace

TEST(IsotopeArithmetic, BaselineErrorExcludedByDefault) {
  IsotopeSignal s = plain_signal();
  ASSERT_FALSE(s.include_baseline_error);
  const VariableId bs = s.baseline.variable_id();
  const VariableId ic = s.intercept.variable_id();

  const UFloat excluded = baseline_corrected(s);
  EXPECT_DOUBLE_EQ(excluded.nominal(), 95.0);
  EXPECT_EQ(excluded.derivative(bs), 0.0);
  EXPECT_EQ(excluded.derivative(ic), 1.0);
  EXPECT_EQ(excluded.terms().size(), 1U);
  EXPECT_EQ(corrected_intensity(s).derivative(bs), 0.0);

  s.include_baseline_error = true;
  const UFloat included = baseline_corrected(s);
  EXPECT_DOUBLE_EQ(included.nominal(), 95.0);
  EXPECT_EQ(included.derivative(bs), -1.0);
  EXPECT_EQ(included.derivative(ic), 1.0);
}

TEST(IsotopeArithmetic, BlankSkippedWhenNotCorrecting) {
  IsotopeSignal s = plain_signal();
  s.discrimination = 1.0;
  s.ic_factor = 1.0;
  const VariableId bk = s.blank.variable_id();

  EXPECT_DOUBLE_EQ(corrected_intensity(s).nominal(), 92.0);
  EXPECT_EQ(corrected_intensity(s).derivative(bk), -1.0);

  s.correct_for_blank = false;
  const UFloat v = corrected_intensity(s);
  EXPECT_DOUBLE_EQ(v.nominal(), 95.0);
  EXPECT_EQ(v.derivative(bk), 0.0);
}

TEST(IsotopeArithmetic, OrderIsSubtractThenDiscThenIc) {
  const IsotopeSignal s = plain_signal();
  const double d = s.discrimination.nominal();
  const double icf = s.ic_factor.nominal();
  const double nd = 100.0 - 5.0 - 3.0;
  const UFloat v = corrected_intensity(s);
  EXPECT_DOUBLE_EQ(v.nominal(), nd * d * icf);
  EXPECT_DOUBLE_EQ(v.derivative(s.blank.variable_id()), -d * icf);
  EXPECT_DOUBLE_EQ(v.derivative(s.intercept.variable_id()), d * icf);
  EXPECT_DOUBLE_EQ(v.derivative(s.discrimination.variable_id()), nd * icf);
  EXPECT_DOUBLE_EQ(v.derivative(s.ic_factor.variable_id()), nd * d);
}

TEST(IsotopeArithmetic, IcFactorZeroHonoured) {
  IsotopeSignal s = plain_signal();
  s.ic_factor = 0.0;  // spec Q18: honoured, not replaced by 1
  const UFloat v = corrected_intensity(s);
  EXPECT_EQ(v.nominal(), 0.0);
  EXPECT_EQ(v.std_dev(), 0.0);

  s.ic_factor = var(0.0, 0.01);
  const UFloat w = corrected_intensity(s);
  EXPECT_EQ(w.nominal(), 0.0);
  EXPECT_DOUBLE_EQ(w.derivative(s.ic_factor.variable_id()), 92.0 * 1.003);
}

TEST(IsotopeArithmetic, AbundanceSensitivityNeighbours) {
  const std::array<UFloat, 5> s{var(1000.0, 1.0), var(100.0, 0.5), var(10.0, 0.05),
                                var(5.0, 0.01), var(2.0, 0.01)};
  const double s40 = 1000.0, s39 = 100.0, s38 = 10.0, s37 = 5.0, s36 = 2.0;
  const double a = 1e-4;
  const auto n = abundance_sensitivity_correction(s, a);
  EXPECT_DOUBLE_EQ(n[0].nominal(), s40 - a * (s39 + s39));  // 2*s39 (spec Q9)
  EXPECT_DOUBLE_EQ(n[1].nominal(), s39 - a * (s40 + s38));
  EXPECT_DOUBLE_EQ(n[2].nominal(), s38 - a * (s39 + s37));
  EXPECT_DOUBLE_EQ(n[3].nominal(), s37 - a * (s38 + s36));
  EXPECT_DOUBLE_EQ(n[4].nominal(), s36 - a * (s37 + s37));  // 2*s37 (spec Q9)
  EXPECT_DOUBLE_EQ(n[0].derivative(s[1].variable_id()), -2.0 * a);
  EXPECT_DOUBLE_EQ(n[4].derivative(s[3].variable_id()), -2.0 * a);
  EXPECT_EQ(n[0].derivative(s[2].variable_id()), 0.0);  // no 41 neighbour
  EXPECT_EQ(n[4].derivative(s[2].variable_id()), 0.0);  // no 35 neighbour
  EXPECT_DOUBLE_EQ(n[2].derivative(s[1].variable_id()), -a);
  EXPECT_DOUBLE_EQ(n[2].derivative(s[3].variable_id()), -a);

  const auto same = abundance_sensitivity_correction(s, 0.0);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(same[i].nominal(), s[i].nominal()) << i;
    ASSERT_EQ(same[i].terms().size(), 1U) << i;
    EXPECT_EQ(same[i].terms()[0].id, s[i].variable_id()) << i;
    EXPECT_EQ(same[i].terms()[0].deriv, 1.0) << i;
  }
}

TEST(IsotopeArithmetic, DeadtimeFormulaAndDomain) {
  EXPECT_EQ(kFaToCountsPerSecond, 6241.509);
  const UFloat s = var(1000.0, 1.0);
  const double tau = 2e-8;
  const double n = 1000.0 * 6241.509;
  const auto r = deadtime_correct(s, tau);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_NEAR(r->nominal(), (n / (1.0 - n * tau)) / 6241.509, 1e-12 * 1142.0);
  const double dd = 1.0 / ((1.0 - n * tau) * (1.0 - n * tau));
  EXPECT_NEAR(r->derivative(s.variable_id()), dd, 1e-12 * dd);

  // n tau >= 1 is outside the domain (spec 6: error if 1 - n tau <= 0).
  const double tau_edge = 1.0 / n;
  const auto edge = deadtime_correct(s, tau_edge * 1.0000001);
  ASSERT_FALSE(edge);
  EXPECT_EQ(edge.error().kind, pychron::ErrorKind::Config);
  EXPECT_NE(edge.error().what.find("reduction: "), std::string::npos);
  EXPECT_NE(edge.error().what.find("deadtime"), std::string::npos);
  EXPECT_FALSE(deadtime_correct(s, 1e-3));
  EXPECT_FALSE(deadtime_correct(s, -1e-8));  // negative tau rejected
  EXPECT_FALSE(deadtime_correct(s, std::nan("")));

  // tau absent (D4: off by default) -> the intercept is used unchanged.
  IsotopeSignal sig = plain_signal();
  ASSERT_FALSE(sig.deadtime_tau_s.has_value());
  const auto same = deadtime_corrected_intercept(sig);
  ASSERT_TRUE(same);
  EXPECT_EQ(same->nominal(), sig.intercept.nominal());
  ASSERT_EQ(same->terms().size(), 1U);
  EXPECT_EQ(same->terms()[0].id, sig.intercept.variable_id());
  EXPECT_EQ(same->terms()[0].deriv, 1.0);

  sig.deadtime_tau_s = tau;
  sig.intercept = s;
  const auto on = deadtime_corrected_intercept(sig);
  ASSERT_TRUE(on);
  EXPECT_EQ(on->nominal(), r->nominal());
  EXPECT_EQ(on->derivative(s.variable_id()), r->derivative(s.variable_id()));

  sig.deadtime_tau_s = 1e-3;
  EXPECT_FALSE(deadtime_corrected_intercept(sig));
}

TEST(IsotopeArithmetic, DeadtimeLegacy6240Divergence) {
  const golden::Json doc = golden::load("isotope_arithmetic.json");
  const golden::Json& c = find_case(doc, "deadtime/legacy_6240");
  ASSERT_TRUE(c.is_object());
  const golden::Json& in = c["inputs"];
  const golden::Tol t = golden::tol_of(c);
  const UFloat s = ufloat_of(in["signal"]);
  const double tau = in["tau_s"].as_number();
  ASSERT_EQ(in["legacy_fa_to_cps"].as_number(), 6240.0);

  const auto legacy = deadtime_correct(s, tau, 6240.0);
  ASSERT_TRUE(legacy);
  expect_ufloat(*legacy, c["legacy_sentinel"]["corrected"], t, "6240 vs legacy_sentinel");

  const auto ours = deadtime_correct(s, tau);  // default 6241.509 (D4)
  ASSERT_TRUE(ours);
  expect_ufloat(*ours, c["expected"]["corrected"], t, "default vs expected");
  EXPECT_TRUE(differs(*ours, c["legacy_sentinel"]["corrected"], t))
      << "default factor should diverge from the legacy 6240 result";
}

TEST(IsotopeArithmetic, Golden) {
  const golden::Json doc = golden::load("isotope_arithmetic.json");
  ASSERT_GT(doc["cases"].size(), 0U);
  for (const golden::Json& c : doc["cases"].as_array()) {
    const std::string name = c["name"].as_string();
    SCOPED_TRACE(name);
    const golden::Json& in = c["inputs"];
    const golden::Json& ex = c["expected"];
    const golden::Tol t = golden::tol_of(c);
    const std::string fn = in["function"].as_string();

    if (fn == "isotope") {
      const std::string iso = in["isotope"].as_string();
      IsotopeSignal s;
      s.intercept = ufloat_of(in["intercept"], iso);
      s.baseline = ufloat_of(in["baseline"], iso + " bs");
      s.blank = ufloat_of(in["blank"], iso + " bk");
      s.ic_factor = ufloat_of(in["ic_factor"], iso + " IC");
      s.discrimination = ufloat_of(in["discrimination"]);
      s.include_baseline_error = in["include_baseline_error"].as_bool();
      s.correct_for_blank = in["correct_for_blank"].as_bool();
      expect_ufloat(baseline_corrected(s), ex["baseline_corrected"], t, "baseline_corrected");
      expect_ufloat(non_detector_corrected(s), ex["non_detector_corrected"], t,
                    "non_detector_corrected");
      expect_ufloat(corrected_intensity(s), ex["intensity"], t, "intensity");
    } else if (fn == "abundance_sensitivity") {
      static constexpr std::array<const char*, 5> kKeys{"Ar40", "Ar39", "Ar38", "Ar37", "Ar36"};
      std::array<UFloat, 5> s;
      for (std::size_t i = 0; i < 5; ++i) s[i] = ufloat_of(in["signals"][kKeys[i]], kKeys[i]);
      const auto n = abundance_sensitivity_correction(s, in["abundance_sensitivity"].as_number());
      for (std::size_t i = 0; i < 5; ++i) expect_ufloat(n[i], ex[kKeys[i]], t, kKeys[i]);
    } else if (fn == "deadtime") {
      const UFloat s = ufloat_of(in["signal"]);
      const auto r = deadtime_correct(s, in["tau_s"].as_number(), in["fa_to_cps"].as_number());
      if (!c["expect_error"].is_null()) {
        ASSERT_FALSE(r);
        EXPECT_NE(r.error().what.find(c["expect_error"].as_string()), std::string::npos)
            << r.error().what;
        continue;
      }
      ASSERT_TRUE(r) << r.error().what;
      expect_ufloat(*r, ex["corrected"], t, "corrected");
      if (in.contains("legacy_fa_to_cps")) {
        const auto l = deadtime_correct(s, in["tau_s"].as_number(),
                                        in["legacy_fa_to_cps"].as_number());
        ASSERT_TRUE(l);
        expect_ufloat(*l, c["legacy_sentinel"]["corrected"], t, "legacy_sentinel");
      }
    } else {
      ADD_FAILURE() << "unknown function " << fn;
    }
  }
}
