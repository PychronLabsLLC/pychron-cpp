// UFloat core (spec 4.1-4.5): derivative tracking, identity, tags, and parity
// with `uncertainties` on the arithmetic-only golden programs (spec 9.3).
#include "pychron/reduction/ufloat.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "golden.hpp"

namespace golden = pychron::reduction::golden;
using pychron::reduction::intern_tag;
using pychron::reduction::tag_name;
using pychron::reduction::UFloat;
using pychron::reduction::VariableId;

TEST(UFloat, VariableHasUnitDerivativeAndUniqueId) {
  const UFloat a = UFloat::variable(1, 0.1);
  const UFloat b = UFloat::variable(1, 0.1);
  EXPECT_NE(a.variable_id(), 0u);
  EXPECT_NE(b.variable_id(), 0u);
  EXPECT_NE(a.variable_id(), b.variable_id());
  EXPECT_EQ(a.derivative(a.variable_id()), 1.0);
  EXPECT_EQ(a.derivative(b.variable_id()), 0.0);
  EXPECT_EQ(a.std_dev(), 0.1);
  EXPECT_EQ(a.nominal(), 1.0);
  ASSERT_EQ(a.terms().size(), 1u);
  EXPECT_EQ(a.terms()[0].sigma, 0.1);
}

TEST(UFloat, ZeroSigmaIsExact) {
  const UFloat v = UFloat::variable(5, 0);
  EXPECT_TRUE(v.is_exact());
  EXPECT_EQ(v.nominal(), 5.0);
  EXPECT_EQ(v.variable_id(), 0u);
  EXPECT_TRUE(UFloat(3.0).terms().empty());
  EXPECT_EQ(UFloat(3.0).std_dev(), 0.0);
  EXPECT_TRUE(UFloat().is_exact());
  EXPECT_EQ(UFloat().nominal(), 0.0);
}

TEST(UFloat, SelfCancellation) {
  const double sigma = 0.1;
  const UFloat x = UFloat::variable(2.0, sigma);

  // NOLINTNEXTLINE(misc-redundant-expression): a value against itself is the case: the correlation must cancel
  const UFloat diff = x - x;
  EXPECT_EQ(diff.nominal(), 0.0);
  EXPECT_TRUE(diff.is_exact());
  EXPECT_TRUE(diff.terms().empty());

  // NOLINTNEXTLINE(misc-redundant-expression): as above
  const UFloat ratio = x / x;
  EXPECT_EQ(ratio.nominal(), 1.0);
  EXPECT_EQ(ratio.std_dev(), 0.0);

  EXPECT_EQ((x + x).std_dev(), 2 * sigma);
}

TEST(UFloat, ProductAndQuotientRules) {
  const UFloat x = UFloat::variable(2.0, 0.1);
  const UFloat y = UFloat::variable(3.0, 0.2);
  const VariableId ix = x.variable_id();
  const VariableId iy = y.variable_id();

  const UFloat p = x * y;  // d/dx = y0, d/dy = x0
  EXPECT_EQ(p.nominal(), 6.0);
  EXPECT_DOUBLE_EQ(p.derivative(ix), 3.0);
  EXPECT_DOUBLE_EQ(p.derivative(iy), 2.0);

  const UFloat q = x / y;  // d/dx = 1/y0, d/dy = -x0/y0^2
  EXPECT_DOUBLE_EQ(q.nominal(), 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(q.derivative(ix), 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(q.derivative(iy), -2.0 / 9.0);

  const UFloat r = 2 / x;  // d/dx = -2/x0^2
  EXPECT_DOUBLE_EQ(r.nominal(), 1.0);
  EXPECT_DOUBLE_EQ(r.derivative(ix), -0.5);

  const UFloat s = x * 3;  // d/dx = 3
  EXPECT_DOUBLE_EQ(s.nominal(), 6.0);
  EXPECT_DOUBLE_EQ(s.derivative(ix), 3.0);
  EXPECT_DOUBLE_EQ(s.std_dev(), 0.3);

  const UFloat n = -x;
  EXPECT_EQ(n.nominal(), -2.0);
  EXPECT_EQ(n.derivative(ix), -1.0);

  UFloat c = x;
  c += y;
  c -= 1.0;
  c *= 2.0;
  c /= x;  // (2(x + y - 1)) / x
  EXPECT_DOUBLE_EQ(c.nominal(), 4.0);
  EXPECT_DOUBLE_EQ(c.derivative(ix), 2.0 / 2.0 - 8.0 / 4.0);
  EXPECT_DOUBLE_EQ(c.derivative(iy), 1.0);
}

TEST(UFloat, TermsSortedAndMergedOnce) {
  const UFloat a = UFloat::variable(1.0, 0.1);
  const UFloat b = UFloat::variable(2.0, 0.2);
  const UFloat sum = b + a + b;  // built in non-id order on purpose
  ASSERT_EQ(sum.terms().size(), 2u);
  EXPECT_LT(sum.terms()[0].id, sum.terms()[1].id);
  EXPECT_EQ(sum.derivative(b.variable_id()), 2.0);
  EXPECT_EQ(sum.derivative(a.variable_id()), 1.0);
  EXPECT_EQ(sum.variable_id(), 0u);

  const UFloat aba = a + b + a;
  ASSERT_EQ(aba.terms().size(), 2u);
  EXPECT_LT(aba.terms()[0].id, aba.terms()[1].id);
  EXPECT_EQ(aba.derivative(a.variable_id()), 2.0);
}

TEST(UFloat, TagsInterned) {
  EXPECT_EQ(intern_tag("J"), intern_tag("J"));
  EXPECT_NE(intern_tag("J"), 0u);
  EXPECT_NE(intern_tag("J"), intern_tag("K"));
  EXPECT_EQ(tag_name(intern_tag("J")), "J");
  EXPECT_EQ(tag_name(0), "");
  EXPECT_EQ(tag_name(0xFFFFFFFFu), "");

  const UFloat j = UFloat::variable(0.01, 1e-5, "J");
  ASSERT_EQ(j.terms().size(), 1u);
  EXPECT_EQ(j.terms()[0].tag, intern_tag("J"));
}

TEST(UFloat, IdsUniqueAcrossThreads) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 10000;
  std::vector<std::vector<VariableId>> ids(kThreads);
  {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&ids, t] {
        ids[static_cast<std::size_t>(t)].reserve(kPerThread);
        for (int i = 0; i < kPerThread; ++i) {
          ids[static_cast<std::size_t>(t)].push_back(UFloat::variable(1.0, 0.1).variable_id());
        }
      });
    }
    for (auto& th : threads) th.join();
  }
  std::vector<VariableId> all;
  for (const auto& v : ids) all.insert(all.end(), v.begin(), v.end());
  ASSERT_EQ(all.size(), static_cast<std::size_t>(kThreads * kPerThread));
  EXPECT_EQ(std::count(all.begin(), all.end(), VariableId{0}), 0);
  std::sort(all.begin(), all.end());
  EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end());
}

TEST(UFloat, SelfAliasingCompoundAssignment) {
  UFloat x = UFloat::variable(2.0, 0.1);
  const VariableId ix = x.variable_id();
  x += x;
  EXPECT_EQ(x.nominal(), 4.0);
  EXPECT_EQ(x.derivative(ix), 2.0);
  ASSERT_EQ(x.terms().size(), 1u);
}

TEST(UFloat, MultiplyByZeroIsExact) {
  const UFloat x = UFloat::variable(2.0, 0.1);
  const UFloat z = x * 0.0;
  EXPECT_EQ(z.nominal(), 0.0);
  EXPECT_TRUE(z.is_exact());
  EXPECT_TRUE(z.terms().empty());
}

TEST(UFloat, DivisionByZeroIsIeee) {
  // Spec 4.3: division by an exact-zero nominal yields inf/NaN, never throws.
  const UFloat x = UFloat::variable(2.0, 0.1);
  const VariableId ix = x.variable_id();

  const UFloat a = x / 0.0;
  EXPECT_TRUE(std::isinf(a.nominal()));
  EXPECT_TRUE(std::isinf(a.derivative(ix)));

  const UFloat b = x / UFloat(0.0);
  EXPECT_TRUE(std::isinf(b.nominal()));
  EXPECT_TRUE(std::isinf(b.derivative(ix)));

  const UFloat zero = UFloat::variable(0.0, 0.1);
  EXPECT_TRUE(std::isnan((zero / 0.0).nominal()));
  EXPECT_TRUE(std::isnan((zero / UFloat(0.0)).nominal()));
}

TEST(UFloat, NanNominalPropagates) {
  const UFloat n = UFloat::variable(std::nan(""), 0.1);
  const UFloat y = UFloat::variable(3.0, 0.2);
  EXPECT_TRUE(std::isnan((n + 1.0).nominal()));
  EXPECT_TRUE(std::isnan((n - y).nominal()));
  const UFloat p = n * y;
  EXPECT_TRUE(std::isnan(p.nominal()));
  EXPECT_TRUE(std::isnan(p.derivative(y.variable_id())));  // NaN != 0, so kept
  EXPECT_TRUE(std::isnan(p.std_dev()));
  EXPECT_TRUE(std::isnan((y / n).nominal()));
}

TEST(UFloat, FunctionDerivatives) {
  const UFloat x = UFloat::variable(0.5, 0.01);
  const VariableId ix = x.variable_id();

  EXPECT_DOUBLE_EQ(exp(x).nominal(), std::exp(0.5));
  EXPECT_DOUBLE_EQ(exp(x).derivative(ix), std::exp(0.5));
  EXPECT_DOUBLE_EQ(log(x).nominal(), std::log(0.5));
  EXPECT_DOUBLE_EQ(log(x).derivative(ix), 2.0);
  EXPECT_DOUBLE_EQ(log10(x).nominal(), std::log10(0.5));
  EXPECT_DOUBLE_EQ(log10(x).derivative(ix), 1.0 / (0.5 * std::log(10.0)));
  EXPECT_DOUBLE_EQ(sqrt(x).nominal(), std::sqrt(0.5));
  EXPECT_DOUBLE_EQ(sqrt(x).derivative(ix), 1.0 / (2.0 * std::sqrt(0.5)));
  EXPECT_DOUBLE_EQ(pow(x, 2.5).nominal(), std::pow(0.5, 2.5));
  EXPECT_DOUBLE_EQ(pow(x, 2.5).derivative(ix), 2.5 * std::pow(0.5, 1.5));

  const UFloat neg = UFloat::variable(-3.0, 0.1);
  EXPECT_EQ(abs(neg).nominal(), 3.0);
  EXPECT_EQ(abs(neg).derivative(neg.variable_id()), -1.0);
  EXPECT_EQ(abs(x).derivative(ix), 1.0);

  const UFloat a = UFloat::variable(2.0, 0.1);
  const UFloat b = UFloat::variable(3.0, 0.2);
  const UFloat p = pow(a, b);  // d/da = b0 a0^(b0-1), d/db = ln(a0) a0^b0
  EXPECT_DOUBLE_EQ(p.nominal(), 8.0);
  EXPECT_DOUBLE_EQ(p.derivative(a.variable_id()), 12.0);
  EXPECT_DOUBLE_EQ(p.derivative(b.variable_id()), std::log(2.0) * 8.0);

  const UFloat q = pow(2, b);  // d/db = ln(2) 2^b0
  EXPECT_DOUBLE_EQ(q.nominal(), 8.0);
  EXPECT_DOUBLE_EQ(q.derivative(b.variable_id()), std::log(2.0) * 8.0);

  const UFloat one = pow(x, 0);
  EXPECT_EQ(one.nominal(), 1.0);
  EXPECT_TRUE(one.is_exact());

  // Spec 4.3: x0 == 0 and c > 1 gives derivative 0.
  const UFloat z = UFloat::variable(0.0, 0.1);
  const UFloat z2 = pow(z, 2);
  EXPECT_EQ(z2.nominal(), 0.0);
  EXPECT_EQ(z2.derivative(z.variable_id()), 0.0);
  EXPECT_TRUE(z2.is_exact());
  EXPECT_EQ(pow(z, 2.5).derivative(z.variable_id()), 0.0);
  EXPECT_EQ(pow(z, 1.0).derivative(z.variable_id()), 1.0);
  // pow(UFloat, UFloat) at x0 == 0 agrees with pow(UFloat, double).
  const UFloat w = UFloat::variable(2.0, 0.1);
  const UFloat zw = pow(z, w);
  EXPECT_EQ(zw.nominal(), 0.0);
  EXPECT_TRUE(zw.is_exact());
}

TEST(UFloat, DomainIsIeee) {
  const UFloat l = log(UFloat::variable(-1.0, 0.1));
  EXPECT_TRUE(std::isnan(l.nominal()));
  const UFloat d = UFloat::variable(1.0, 0.1) / UFloat(0);
  EXPECT_FALSE(std::isfinite(d.nominal()));
  EXPECT_TRUE(std::isnan(sqrt(UFloat::variable(-4.0, 0.1)).nominal()));
}

TEST(UFloat, CovarianceViaSharedVariables) {
  using pychron::reduction::correlation;
  using pychron::reduction::covariance;
  using pychron::reduction::covariance_matrix;
  const double sx = 0.1;
  const double sy = 0.2;
  const UFloat x = UFloat::variable(2.0, sx);
  const UFloat y = UFloat::variable(3.0, sy);
  const UFloat a = x + y;
  const UFloat b = x - y;
  EXPECT_DOUBLE_EQ(covariance(a, b), sx * sx - sy * sy);
  EXPECT_DOUBLE_EQ(covariance(b, a), sx * sx - sy * sy);
  EXPECT_DOUBLE_EQ(covariance(a, a), a.variance());
  EXPECT_EQ(covariance(x, y), 0.0);
  EXPECT_EQ(covariance(x, UFloat(4.0)), 0.0);
  EXPECT_DOUBLE_EQ(correlation(a, b), (sx * sx - sy * sy) / (sx * sx + sy * sy));
  EXPECT_EQ(correlation(a, UFloat(1.0)), 0.0);
  EXPECT_EQ(correlation(x, y), 0.0);

  const std::vector<UFloat> xs{a, b, x, y};
  const std::vector<double> m = covariance_matrix(xs);
  ASSERT_EQ(m.size(), 16u);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(m[i * 4 + i], xs[i].variance());
    for (std::size_t j = 0; j < 4; ++j) {
      EXPECT_EQ(m[i * 4 + j], m[j * 4 + i]);
      EXPECT_DOUBLE_EQ(m[i * 4 + j], covariance(xs[i], xs[j]));
    }
  }
  EXPECT_TRUE(covariance_matrix(std::vector<UFloat>{}).empty());
}

TEST(UFloat, ExclusionAndComponents) {
  using pychron::reduction::error_components;
  using pychron::reduction::std_dev_excluding;
  using pychron::reduction::std_dev_excluding_tags;
  using pychron::reduction::TagId;
  using pychron::reduction::variance_percent;
  const TagId tx = intern_tag("budget_x");
  const TagId ty = intern_tag("budget_y");
  const UFloat x = UFloat::variable(2.0, 0.1, tx);
  const UFloat y = UFloat::variable(3.0, 0.2, ty);
  const UFloat z = UFloat::variable(5.0, 0.3, tx);
  const UFloat f = x * y + z;

  const std::vector<VariableId> drop_y{y.variable_id()};
  const UFloat rebuilt = x * UFloat(3.0) + z;
  EXPECT_DOUBLE_EQ(std_dev_excluding(f, drop_y), rebuilt.std_dev());
  EXPECT_DOUBLE_EQ(std_dev_excluding(f, std::vector<VariableId>{}), f.std_dev());
  // Exact values report variable id 0 and have no terms; an id 0 in the
  // exclusion list (E15 with zero-error ratios) excludes nothing.
  EXPECT_EQ(UFloat::variable(0.5, 0.0).variable_id(), VariableId{0});
  EXPECT_EQ(std_dev_excluding(f, std::vector<VariableId>{0}), f.std_dev());
  EXPECT_EQ(std_dev_excluding(f, std::vector<VariableId>{0, y.variable_id(), 0}),
            std_dev_excluding(f, drop_y));

  const std::vector<TagId> drop_tx{tx};
  EXPECT_DOUBLE_EQ(std_dev_excluding_tags(f, drop_tx), 2.0 * 0.2);

  const auto comps = error_components(f);
  ASSERT_EQ(comps.size(), 2u);
  EXPECT_LT(comps[0].first, comps[1].first);
  for (const auto& [tag, err] : comps) {
    if (tag == tx) {
      EXPECT_DOUBLE_EQ(err, std::sqrt(0.3 * 0.3 + 0.3 * 0.3));  // (y0 sx), sz in quadrature
    } else {
      EXPECT_EQ(tag, ty);
      EXPECT_DOUBLE_EQ(err, 0.4);
    }
  }

  const double pct = variance_percent(f, tx) + variance_percent(f, ty);
  EXPECT_NEAR(pct, 100.0, 1e-12);
  EXPECT_DOUBLE_EQ(variance_percent(f, ty), 100.0 * 0.16 / f.variance());
  EXPECT_EQ(variance_percent(f, intern_tag("budget_absent")), 0.0);
  EXPECT_EQ(variance_percent(UFloat(1.0), tx), 0.0);
  EXPECT_TRUE(error_components(UFloat(1.0)).empty());
}

TEST(UFloat, CopiesShareVariables) {
  using pychron::reduction::covariance;
  const UFloat j = UFloat::variable(0.01, 1e-5, "J");
  const UFloat j_copy = j;  // NOLINT(performance-unnecessary-copy-initialization)
  const UFloat a = j * 2.0 + UFloat::variable(1.0, 0.1);
  const UFloat b = j_copy * 3.0 + UFloat::variable(1.0, 0.1);
  EXPECT_NE(covariance(a, b), 0.0);
  EXPECT_DOUBLE_EQ(covariance(a, b), 6.0 * 1e-5 * 1e-5);
}

namespace {

bool arithmetic_only(const golden::Json& c) {
  for (const auto& op : c["inputs"]["ops"].as_array()) {
    const std::string& s = op.as_string();
    if (s != "add" && s != "sub" && s != "mul" && s != "div" && s != "neg") return false;
  }
  return true;
}

using Env = std::map<std::string, UFloat, std::less<>>;

// Looks up `key`; a missing key is a test failure and yields exact 0.
const UFloat& lookup(const Env& env, const std::string& key) {
  static const UFloat kMissing;
  auto it = env.find(key);
  if (it == env.end()) {
    ADD_FAILURE() << "unknown variable " << key;
    return kMissing;
  }
  return it->second;
}

// Evaluates a ufloat.json program (spec 9.3): `vars` become independent
// variables, then each step [out, op, lhs, rhs?] runs in order.
Env run_program(const golden::Json& in) {
  Env env;
  for (const auto& [var, spec] : in["vars"].as_object()) {
    env[var] = UFloat::variable(spec["v"].as_number(), spec["e"].as_number(),
                                spec["tag"].as_string());
  }
  auto operand = [&env](const golden::Json& j) -> UFloat {
    if (j.is_number()) return UFloat(j.as_number());
    return lookup(env, j.as_string());
  };
  for (const auto& step : in["steps"].as_array()) {
    const std::string& out = step[0].as_string();
    const std::string& op = step[1].as_string();
    const UFloat lhs = operand(step[2]);
    // Exercise the double overloads when an operand is a literal number.
    const bool ln = step[2].is_number();
    const bool rn = step[3].is_number();
    const double lv = ln ? step[2].as_number() : 0.0;
    const double rv = rn ? step[3].as_number() : 0.0;
    UFloat r;
    if (op == "neg") {
      r = -lhs;
    } else if (op == "exp") {
      r = exp(lhs);
    } else if (op == "log") {
      r = log(lhs);
    } else if (op == "log10") {
      r = log10(lhs);
    } else if (op == "sqrt") {
      r = sqrt(lhs);
    } else if (op == "abs") {
      r = abs(lhs);
    } else {
      const UFloat rhs = operand(step[3]);
      if (op == "add") {
        r = ln ? lv + rhs : rn ? lhs + rv : lhs + rhs;
      } else if (op == "sub") {
        r = ln ? lv - rhs : rn ? lhs - rv : lhs - rhs;
      } else if (op == "mul") {
        r = ln ? lv * rhs : rn ? lhs * rv : lhs * rhs;
      } else if (op == "div") {
        r = ln ? lv / rhs : rn ? lhs / rv : lhs / rhs;
      } else if (op == "pow") {
        r = ln ? pow(lv, rhs) : rn ? pow(lhs, rv) : pow(lhs, rhs);
      } else {
        ADD_FAILURE() << "unexpected op " << op;
      }
    }
    env[out] = r;
  }
  return env;
}

// Spec 4.3 divergence: legacy `uncertainties` records a NaN derivative for
// z**2.5 at z = 0; spec 4.3 defines pow(x, c) with x0 == 0 and c > 1 as
// derivative 0 (see this case's inputs.note). The golden file is not edited;
// this test asserts the spec value and that the golden still records NaN.
constexpr const char* kPowAtZeroNonInteger = "ufloat/func/pow_at_zero_non_integer";

void check_case(const golden::Json& c) {
  using pychron::reduction::covariance;
  const std::string& name = c["name"].as_string();
  SCOPED_TRACE(name);
  const golden::Tol tol = golden::tol_of(c);
  const Env env = run_program(c["inputs"]);
  const golden::Json& expected = c["expected"];

  if (name == kPowAtZeroNonInteger) {
    EXPECT_FALSE(c["inputs"]["note"].as_string().empty());
    // Inputs: z = 0 +/- 0.1, a = z ** 2.5.
    EXPECT_TRUE(std::isnan(expected["a"]["e"].as_number()));  // legacy NaN
    const UFloat& a = lookup(env, "a");
    golden::expect_close(a.nominal(), expected["a"]["v"].as_number(), tol.rtol, tol.atol,
                         name + " a.v");
    EXPECT_EQ(a.std_dev(), 0.0);       // spec 4.3: derivative 0
    EXPECT_EQ(covariance(a, a), 0.0);  // legacy cov(a, a) = NaN
    return;
  }

  int checked = 0;
  for (const auto& [key, want] : expected.as_object()) {
    if (key == "cov") continue;
    const UFloat& got = lookup(env, key);
    golden::expect_close(got.nominal(), want["v"].as_number(), tol.rtol, tol.atol,
                         name + " " + key + ".v");
    golden::expect_close(got.std_dev(), want["e"].as_number(), tol.rtol_err, tol.atol_err,
                         name + " " + key + ".e");
    ++checked;
  }
  EXPECT_GT(checked, 0);
  for (const auto& entry : expected["cov"].as_array()) {
    const std::string& a = entry[0].as_string();
    const std::string& b = entry[1].as_string();
    golden::expect_close(covariance(lookup(env, a), lookup(env, b)), entry[2].as_number(),
                         tol.rtol_err, tol.atol_err, name + " cov(" + a + "," + b + ")");
  }
}

}  // namespace

TEST(UFloat, GoldenArithmetic) {
  const golden::Json doc = golden::load("ufloat.json");
  int ran = 0;
  for (const auto& c : doc["cases"].as_array()) {
    if (!arithmetic_only(c)) continue;
    ++ran;
    check_case(c);
  }
  EXPECT_EQ(ran, 6);
}

TEST(UFloat, GoldenFunctionsAndCovariance) {
  const golden::Json doc = golden::load("ufloat.json");
  int ran = 0;
  bool saw_divergence = false;
  for (const auto& c : doc["cases"].as_array()) {
    if (arithmetic_only(c)) continue;
    ++ran;
    saw_divergence = saw_divergence || c["name"].as_string() == kPowAtZeroNonInteger;
    check_case(c);
  }
  EXPECT_EQ(ran, 7);
  EXPECT_TRUE(saw_divergence);
}
