#include "pychron/sim/linear_flow.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/error.hpp"
#include "pychron/sim/gas.hpp"

namespace {

using namespace pychron;
using namespace pychron::sim;

double sum(const std::vector<double>& n) {
  double s = 0.0;
  for (const double v : n) s += v;
  return s;
}

// |a - b| <= rel * max(|a|, |b|).
::testing::AssertionResult near_rel(double a, double b, double rel) {
  const double scale = std::max(std::abs(a), std::abs(b));
  if (std::abs(a - b) <= rel * scale) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure() << a << " and " << b << " differ by " << std::abs(a - b) / scale
                                       << " relative, more than " << rel;
}

// The same numbers from every standard library: the distributions in <random>
// are not specified bit for bit, the engine is.
struct Random {
  std::mt19937 engine{0x5eed};
  double unit() { return static_cast<double>(engine()) / 4294967296.0; }
  double log_uniform(double lo, double hi) { return lo * std::pow(hi / lo, unit()); }
};

LinearFlow must(FlowTerms terms) {
  auto flow = LinearFlow::make(std::move(terms));
  EXPECT_TRUE(flow.has_value()) << (flow ? "" : flow.error().what);
  return std::move(*flow);
}

// Five volumes in a line, 0 - 1 - 2 - 3 - 4, with random conductances.
FlowTerms chain(Random& random) {
  FlowTerms terms;
  terms.volume = {0.05, 0.15, 0.02, 0.5, 0.08};
  for (std::size_t i = 0; i + 1 < terms.volume.size(); ++i) {
    terms.links.push_back({i, i + 1, random.log_uniform(1e-3, 10.0)});
  }
  terms.loss.assign(terms.volume.size(), 0.0);
  terms.source.assign(terms.volume.size(), 0.0);
  return terms;
}

TEST(Gas, AirRatios) {
  const Composition air = air_ratios();
  const double ar36 = air[index(Species::Ar36)];
  const double ar38 = air[index(Species::Ar38)];
  const double ar40 = air[index(Species::Ar40)];
  EXPECT_DOUBLE_EQ(ar40 / ar36, 298.56);
  EXPECT_DOUBLE_EQ(ar38 / ar36, 0.1885);
  EXPECT_DOUBLE_EQ(air[index(Species::Active)], 106 * (ar36 + ar38 + ar40));
  EXPECT_EQ(air[index(Species::Ar37)], 0.0);
  EXPECT_EQ(air[index(Species::Ar39)], 0.0);

  const Composition shot = with_ar40(air_ratios(), 2e-7);
  EXPECT_EQ(shot[index(Species::Ar40)], 2e-7);
  EXPECT_DOUBLE_EQ(shot[index(Species::Ar40)] / shot[index(Species::Ar36)], 298.56);
  EXPECT_DOUBLE_EQ(total(shot), total(air) * (2e-7 / ar40));
}

TEST(Gas, CocktailRatiosAndArithmetic) {
  const Composition cocktail = cocktail_ratios();
  EXPECT_EQ(cocktail[index(Species::Ar36)], 1.0);
  EXPECT_EQ(cocktail[index(Species::Ar37)], 0.5);
  EXPECT_EQ(cocktail[index(Species::Ar38)], 0.1885);
  EXPECT_EQ(cocktail[index(Species::Ar39)], 20.0);
  EXPECT_EQ(cocktail[index(Species::Ar40)], 298.56);
  EXPECT_EQ(cocktail[index(Species::Active)], 0.0);

  EXPECT_DOUBLE_EQ(total(cocktail), 1.0 + 0.5 + 0.1885 + 20.0 + 298.56);
  EXPECT_DOUBLE_EQ(total(scaled(cocktail, 2.0)), 2.0 * total(cocktail));
  EXPECT_EQ(kSpeciesName[index(Species::Active)], "active");
  EXPECT_EQ(kSpeciesMass[index(Species::Ar40)], 39.962);
  // Ratios with no Ar40 have no composition of a given Ar40: nothing, not NaN.
  EXPECT_EQ(total(with_ar40(Composition{}, 2e-7)), 0.0);
}

TEST(LinearFlow, TwoVolumesEquilibrateToTheVolumeWeightedMean) {
  const double v1 = 0.05;
  const double v2 = 0.15;
  const double c = 0.1;
  FlowTerms terms;
  terms.volume = {v1, v2};
  terms.links = {{0, 1, c}};
  terms.loss = {0.0, 0.0};
  terms.source = {0.0, 0.0};
  const LinearFlow flow = must(terms);
  ASSERT_EQ(flow.size(), 2U);

  std::vector<double> n{1e-6 * v1, 0.0};
  flow.advance(n, 100.0);
  EXPECT_TRUE(near_rel(n[0] / v1, 2.5e-7, 1e-12));
  EXPECT_TRUE(near_rel(n[1] / v2, 2.5e-7, 1e-12));

  const double tau = v1 * v2 / ((v1 + v2) * c);
  EXPECT_DOUBLE_EQ(tau, 0.375);
  n = {1e-6 * v1, 0.0};
  flow.advance(n, tau);
  EXPECT_TRUE(near_rel(n[0] / v1 - n[1] / v2, 1e-6 / std::exp(1.0), 1e-9));
}

TEST(LinearFlow, ConservesAmountWithNoLossOrSource) {
  Random random;
  const LinearFlow flow = must(chain(random));
  std::vector<double> n{3e-8, 0.0, 1e-9, 4e-7, 0.0};
  const double before = sum(n);
  double worst = 0.0;
  for (int step = 0; step < 1000; ++step) {
    flow.advance(n, random.log_uniform(1e-6, 1e4));
    worst = std::max(worst, std::abs(sum(n) - before) / before);
    ASSERT_TRUE(near_rel(sum(n), before, 1e-10)) << "step " << step;
  }
  RecordProperty("worst_relative_drift", std::to_string(worst));
  // And it has gone where it should: one pressure everywhere.
  const double volume = 0.05 + 0.15 + 0.02 + 0.5 + 0.08;
  const std::vector<double> volumes{0.05, 0.15, 0.02, 0.5, 0.08};
  for (std::size_t i = 0; i < n.size(); ++i) EXPECT_TRUE(near_rel(n[i] / volumes[i], before / volume, 1e-9));
}

TEST(LinearFlow, OneStepEqualsMany) {
  Random random;
  FlowTerms terms = chain(random);
  terms.loss = {0.0, 2e-5, 0.0, 1e-3, 0.0};
  terms.source = {1e-12, 0.0, 3e-13, 0.0, 5e-11};
  const LinearFlow flow = must(terms);

  const std::vector<double> start{3e-8, 0.0, 1e-9, 4e-7, 0.0};
  std::vector<double> once = start;
  flow.advance(once, 3600.0);
  std::vector<double> many = start;
  for (int i = 0; i < 3600; ++i) flow.advance(many, 1.0);
  for (std::size_t i = 0; i < once.size(); ++i) {
    EXPECT_GT(once[i], 0.0);
    EXPECT_TRUE(near_rel(once[i], many[i], 1e-9)) << "volume " << i;
  }
}

TEST(LinearFlow, PumpedVolumeDecaysToItsBase) {
  const double v = 0.05;
  const double speed = 50.0;
  const double base = 1e-9;
  const double p0 = 1e-6;
  FlowTerms terms;
  terms.volume = {v};
  terms.loss = {speed / v};
  terms.source = {base * speed};
  const LinearFlow flow = must(terms);
  ASSERT_EQ(flow.size(), 1U);

  for (const double t : {0.0005, 0.001, 0.01}) {
    std::vector<double> n{p0 * v};
    flow.advance(n, t);
    EXPECT_TRUE(near_rel(n[0] / v, base + (p0 - base) * std::exp(-t * speed / v), 1e-9)) << "t = " << t;
  }
  // Long after, it sits at the base and stays there.
  std::vector<double> n{p0 * v};
  flow.advance(n, 3600.0);
  EXPECT_TRUE(near_rel(n[0] / v, base, 1e-12));
  flow.advance(n, 1e9);
  EXPECT_TRUE(near_rel(n[0] / v, base, 1e-12));
}

TEST(LinearFlow, AClosedRegionWithASourceGrowsLinearly) {
  const double va = 0.05;
  const double vb = 0.15;
  const double c = 0.1;
  const double q = 1e-12;
  FlowTerms terms;
  terms.volume = {va, vb};
  terms.links = {{0, 1, c}};
  terms.loss = {0.0, 0.0};
  terms.source = {q, 0.0};
  const LinearFlow flow = must(terms);

  std::vector<double> n{2e-8 * va, 2e-8 * vb};
  const double before = sum(n);
  flow.advance(n, 1e6);
  EXPECT_TRUE(near_rel(sum(n) - before, 1e-6, 1e-9));

  // Equal, but for the gradient that carries the source's gas across the
  // link: of what enters a, the share vb / (va + vb) flows on to b, and a
  // flow f through a conductance c needs a pressure difference f / c. That
  // is 7.5e-12 mbar here against 5e-6: equal to 1.5 parts in a million, and
  // the difference itself is what the exact solution says it is.
  const double pa = n[0] / va;
  const double pb = n[1] / vb;
  EXPECT_TRUE(near_rel(pa, pb, 2e-6));
  EXPECT_NEAR(pa - pb, q * vb / ((va + vb) * c), 1e-9 * pa);
  EXPECT_TRUE(near_rel(pb, (before + 1e-6) / (va + vb), 2e-6));

  // From nothing, too: the zero mode starts at zero.
  n = {0.0, 0.0};
  flow.advance(n, 1e6);
  EXPECT_TRUE(near_rel(sum(n), 1e-6, 1e-9));

  // A source into a volume joined to nothing grows it and only it.
  FlowTerms apart;
  apart.volume = {va, vb};
  apart.loss = {0.0, 0.0};
  apart.source = {0.0, q};
  const LinearFlow alone = must(apart);
  n = {3e-9, 0.0};
  alone.advance(n, 1e6);
  EXPECT_EQ(n[0], 3e-9);
  EXPECT_TRUE(near_rel(n[1], 1e-6, 1e-12));
}

TEST(LinearFlow, StiffAndLongStepsStayFiniteAndNonNegative) {
  // A fast valve, a pinhole, a fast valve, a pinhole: rates 1e9 apart.
  FlowTerms terms;
  terms.volume = {0.05, 0.15, 0.02, 0.5, 0.08};
  terms.links = {{0, 1, 1e3}, {1, 2, 1e-6}, {2, 3, 1e3}, {3, 4, 1e-6}};
  terms.loss.assign(5, 0.0);
  terms.source.assign(5, 0.0);
  const LinearFlow flow = must(terms);

  std::vector<double> n{1e-6 * 0.05, 0.0, 0.0, 0.0, 0.0};
  const double before = sum(n);
  for (const double dt : {1e-6, 604800.0, 1e-6}) {
    flow.advance(n, dt);
    for (std::size_t i = 0; i < n.size(); ++i) {
      EXPECT_TRUE(std::isfinite(n[i])) << "volume " << i << " after " << dt;
      EXPECT_GE(n[i], 0.0) << "volume " << i << " after " << dt;
    }
    EXPECT_TRUE(near_rel(sum(n), before, 1e-9)) << "after " << dt;
  }
  // The fast pair has long been one pressure.
  EXPECT_TRUE(near_rel(n[0] / 0.05, n[1] / 0.15, 1e-9));

  // The same line pumped at one end and outgassing everywhere: nothing to
  // conserve, but nothing may go negative, infinite or not-a-number.
  terms.loss = {0.0, 0.0, 0.0, 0.0, 50.0 / 0.08};
  terms.source = {1e-13, 1e-13, 1e-13, 1e-13, 1e-9 * 50.0};
  const LinearFlow pumped = must(terms);
  n = {1e-6 * 0.05, 0.0, 0.0, 0.0, 0.0};
  for (const double dt : {1e-6, 604800.0, 1e-6, 1e-9, 3.2e9}) {
    pumped.advance(n, dt);
    for (std::size_t i = 0; i < n.size(); ++i) {
      EXPECT_TRUE(std::isfinite(n[i])) << "volume " << i << " after " << dt;
      EXPECT_GE(n[i], 0.0) << "volume " << i << " after " << dt;
    }
  }
}

TEST(LinearFlow, ASlowLossBesideAFastValveKeepsItsRate) {
  // A pipette on a fast valve, 1e7 / s, and beyond it a volume that loses
  // gas at 2e-5 / s, then at 1e-9 / s: rates 1e12 and 1e16 apart. Once the
  // valve has equilibrated, everything decays at the slow eigenvalue of the
  // two-by-two system, which is det / (the fast one) with nothing to cancel.
  const double v0 = 1e-4;
  const double v1 = 1.0;
  const double c = 1e3;
  for (const double loss : {2e-5, 1e-9}) {
    FlowTerms terms;
    terms.volume = {v0, v1};
    terms.links = {{0, 1, c}};
    terms.loss = {0.0, loss};
    terms.source = {0.0, 0.0};
    const LinearFlow flow = must(terms);

    const double trace = -(c / v0 + c / v1 + loss);
    const double det = c * loss / v0;
    const double fast = 0.5 * (trace - std::sqrt(trace * trace - 4.0 * det));
    const double slow = det / fast;

    std::vector<double> n{1e-6 * v0, 0.0};
    flow.advance(n, 1.0);
    const std::vector<double> settled = n;
    EXPECT_TRUE(near_rel(settled[0] / v0, settled[1] / v1, 1e-9));
    for (const double dt : {3600.0, 1e5, 1e7}) {
      n = settled;
      flow.advance(n, dt);
      for (std::size_t i = 0; i < n.size(); ++i) {
        EXPECT_TRUE(near_rel(n[i] / settled[i], std::exp(slow * dt), 1e-12))
            << "loss " << loss << ", volume " << i << ", after " << dt;
      }
    }
  }
}

TEST(LinearFlow, EdgeCases) {
  // No time passes: nothing changes, to the bit.
  Random random;
  FlowTerms terms = chain(random);
  terms.loss = {0.0, 2e-5, 0.0, 1e-3, 0.0};
  terms.source = {1e-12, 0.0, 3e-13, 0.0, 5e-11};
  const LinearFlow flow = must(terms);
  const std::vector<double> start{3e-8, 0.0, 1e-9, 4e-7, 0.0};
  std::vector<double> n = start;
  flow.advance(n, 0.0);
  EXPECT_EQ(n, start);

  // One volume, nothing attached: it keeps what it has.
  FlowTerms one;
  one.volume = {0.05};
  one.loss = {0.0};
  one.source = {0.0};
  const LinearFlow sealed = must(one);
  n = {4e-8};
  sealed.advance(n, 1e7);
  EXPECT_EQ(n[0], 4e-8);

  // Two links between the same volumes are one link of their sum.
  FlowTerms twice;
  twice.volume = {0.05, 0.15};
  twice.links = {{0, 1, 0.04}, {1, 0, 0.06}};
  twice.loss = {0.0, 0.0};
  twice.source = {0.0, 0.0};
  const LinearFlow parallel = must(twice);
  n = {1e-6 * 0.05, 0.0};
  parallel.advance(n, 0.375);
  EXPECT_TRUE(near_rel(n[0] / 0.05 - n[1] / 0.15, 1e-6 / std::exp(1.0), 1e-9));

  // A closed valve is a link of no conductance.
  twice.links = {{0, 1, 0.0}};
  const LinearFlow closed = must(twice);
  n = {1e-6 * 0.05, 0.0};
  closed.advance(n, 1e6);
  EXPECT_EQ(n[0], 1e-6 * 0.05);
  EXPECT_EQ(n[1], 0.0);

  // No volumes at all is a flow of nothing.
  const LinearFlow none = must(FlowTerms{});
  EXPECT_EQ(none.size(), 0U);
  n.clear();
  none.advance(n, 1.0);
  EXPECT_TRUE(n.empty());
}

TEST(LinearFlow, RefusesBadTerms) {
  const auto good = [] {
    FlowTerms terms;
    terms.volume = {0.05, 0.15};
    terms.links = {{0, 1, 0.1}};
    terms.loss = {0.0, 0.0};
    terms.source = {0.0, 0.0};
    return terms;
  };
  const auto refused = [](const FlowTerms& terms) {
    const auto flow = LinearFlow::make(terms);
    if (flow.has_value()) return ::testing::AssertionFailure() << "accepted";
    if (flow.error().kind != ErrorKind::Config) return ::testing::AssertionFailure() << to_string(flow.error());
    return ::testing::AssertionSuccess();
  };
  ASSERT_TRUE(LinearFlow::make(good()).has_value());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  FlowTerms terms = good();
  terms.volume[1] = 0.0;
  EXPECT_TRUE(refused(terms)) << "a zero volume";

  terms = good();
  terms.links[0].b = 2;
  EXPECT_TRUE(refused(terms)) << "a link index out of range";

  terms = good();
  terms.links[0].conductance = -0.1;
  EXPECT_TRUE(refused(terms)) << "a negative conductance";

  terms = good();
  terms.source[0] = nan;
  EXPECT_TRUE(refused(terms)) << "a NaN source";

  terms = good();
  terms.volume[0] = -0.05;
  EXPECT_TRUE(refused(terms)) << "a negative volume";
  terms = good();
  terms.volume[0] = inf;
  EXPECT_TRUE(refused(terms)) << "an infinite volume";
  terms = good();
  terms.loss.pop_back();
  EXPECT_TRUE(refused(terms)) << "a loss per volume, one missing";
  terms = good();
  terms.source.push_back(0.0);
  EXPECT_TRUE(refused(terms)) << "a source per volume, one extra";
  terms = good();
  terms.links[0].b = 0;
  EXPECT_TRUE(refused(terms)) << "a link from a volume to itself";
  terms = good();
  terms.links[0].conductance = inf;
  EXPECT_TRUE(refused(terms)) << "an infinite conductance";
  terms = good();
  terms.loss[1] = -1.0;
  EXPECT_TRUE(refused(terms)) << "a negative loss";
  terms = good();
  terms.loss[1] = nan;
  EXPECT_TRUE(refused(terms)) << "a NaN loss";
  terms = good();
  terms.source[1] = -1e-12;
  EXPECT_TRUE(refused(terms)) << "a negative source";
}

}  // namespace
