// Isotope arithmetic E1-E3 and E5 (spec 3.1, D4) on one signal. The
// isotope_arithmetic.json golden vectors and E4 are in tests/arar.
#include "pychron/reduction/signal.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using namespace pychron::reduction;

namespace {

UFloat var(double v, double e) { return UFloat::variable(v, e); }

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
