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

  const UFloat diff = x - x;
  EXPECT_EQ(diff.nominal(), 0.0);
  EXPECT_TRUE(diff.is_exact());
  EXPECT_TRUE(diff.terms().empty());

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

namespace {

// Test-local covariance over shared variables (spec 4.1); the library
// `covariance` arrives with the UFloat functions task.
double test_covariance(const UFloat& a, const UFloat& b) {
  double cov = 0.0;
  for (const auto& t : a.terms()) {
    const double db = b.derivative(t.id);
    cov += t.deriv * db * t.sigma * t.sigma;
  }
  return cov;
}

bool arithmetic_only(const golden::Json& c) {
  for (const auto& op : c["inputs"]["ops"].as_array()) {
    const std::string& s = op.as_string();
    if (s != "add" && s != "sub" && s != "mul" && s != "div" && s != "neg") return false;
  }
  return true;
}

}  // namespace

TEST(UFloat, GoldenArithmetic) {
  const golden::Json doc = golden::load("ufloat.json");
  int ran = 0;
  for (const auto& c : doc["cases"].as_array()) {
    if (!arithmetic_only(c)) continue;
    const std::string& name = c["name"].as_string();
    SCOPED_TRACE(name);
    ++ran;
    const golden::Tol tol = golden::tol_of(c);
    const golden::Json& in = c["inputs"];

    std::map<std::string, UFloat, std::less<>> env;
    for (const auto& [var, spec] : in["vars"].as_object()) {
      env[var] = UFloat::variable(spec["v"].as_number(), spec["e"].as_number(),
                                  spec["tag"].as_string());
    }
    auto operand = [&env](const golden::Json& j) -> UFloat {
      if (j.is_string()) {
        auto it = env.find(j.as_string());
        if (it == env.end()) {
          ADD_FAILURE() << "unknown operand " << j.as_string();
          return UFloat{};
        }
        return it->second;
      }
      return UFloat(j.as_number());
    };
    for (const auto& step : in["steps"].as_array()) {
      const std::string& out = step[0].as_string();
      const std::string& op = step[1].as_string();
      const UFloat lhs = operand(step[2]);
      UFloat r;
      if (op == "neg") {
        r = -lhs;
      } else {
        const UFloat rhs = operand(step[3]);
        // Exercise the double overloads when an operand is a literal number.
        const bool ln = step[2].is_number() && !step[2].is_string();
        const bool rn = step[3].is_number() && !step[3].is_string();
        const double lv = ln ? step[2].as_number() : 0.0;
        const double rv = rn ? step[3].as_number() : 0.0;
        if (op == "add") {
          r = ln ? lv + rhs : rn ? lhs + rv : lhs + rhs;
        } else if (op == "sub") {
          r = ln ? lv - rhs : rn ? lhs - rv : lhs - rhs;
        } else if (op == "mul") {
          r = ln ? lv * rhs : rn ? lhs * rv : lhs * rhs;
        } else if (op == "div") {
          r = ln ? lv / rhs : rn ? lhs / rv : lhs / rhs;
        } else {
          ADD_FAILURE() << "unexpected op " << op;
        }
      }
      env[out] = r;
    }

    const golden::Json& expected = c["expected"];
    int checked = 0;
    for (const auto& [key, want] : expected.as_object()) {
      if (key == "cov") continue;
      const UFloat& got = env[key];
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
      golden::expect_close(test_covariance(env[a], env[b]), entry[2].as_number(), tol.rtol_err,
                           tol.atol_err, name + " cov(" + a + "," + b + ")");
    }
  }
  EXPECT_EQ(ran, 6);
}
