#include "pychron/sim/keyed_noise.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using pychron::sim::keyed_bits;
using pychron::sim::keyed_gauss;
using pychron::sim::keyed_poisson;

TEST(KeyedNoise, SameKeySameDraw) {
  EXPECT_EQ(keyed_bits(0x5eed, "bone", 42), keyed_bits(0x5eed, "bone", 42));
  EXPECT_EQ(keyed_gauss(0x5eed, "bone", 42), keyed_gauss(0x5eed, "bone", 42));
  // Whatever was drawn in between.
  const double first = keyed_gauss(7, "H1", -3);
  (void)keyed_gauss(7, "H2", -3);
  (void)keyed_gauss(8, "H1", 1000000);
  EXPECT_EQ(keyed_gauss(7, "H1", -3), first);
}

TEST(KeyedNoise, DifferentKeyOrTickDiffers) {
  const std::uint64_t base = keyed_bits(0x5eed, "bone", 42);
  EXPECT_NE(keyed_bits(0x5eee, "bone", 42), base);
  EXPECT_NE(keyed_bits(0x5eed, "bonf", 42), base);
  EXPECT_NE(keyed_bits(0x5eed, "bone", 43), base);
  EXPECT_NE(keyed_bits(0x5eed, "", 42), base);
  EXPECT_NE(keyed_gauss(0x5eed, "prep", 42), keyed_gauss(0x5eed, "bone", 42));
  EXPECT_NE(keyed_gauss(0x5eed, "bone", 43), keyed_gauss(0x5eed, "bone", 42));
  // A run of ticks, and the same run under another key, share no draw.
  std::set<std::uint64_t> seen;
  for (std::int64_t tick = 0; tick < 1000; ++tick) {
    EXPECT_TRUE(seen.insert(keyed_bits(1, "a", tick)).second) << tick;
    EXPECT_TRUE(seen.insert(keyed_bits(1, "b", tick)).second) << tick;
  }
}

TEST(KeyedNoise, GaussHasUnitVariance) {
  const int count = 100000;
  double sum = 0.0;
  double squares = 0.0;
  for (std::int64_t tick = 0; tick < count; ++tick) {
    const double draw = keyed_gauss(0x5eed, "bone", tick);
    ASSERT_TRUE(std::isfinite(draw)) << tick;
    sum += draw;
    squares += draw * draw;
  }
  const double mean = sum / count;
  const double variance = squares / count - mean * mean;
  EXPECT_NEAR(mean, 0.0, 0.01);
  EXPECT_NEAR(variance, 1.0, 0.02);
}

// Mean and variance of `count` draws at ticks 0, 1, ... (about the mean
// asked for, so a mean of 1e9 loses nothing to the squares).
struct Moments {
  double mean = 0.0;
  double variance = 0.0;
};

Moments poisson_moments(std::string_view key, double mean, int count) {
  double sum = 0.0;
  double squares = 0.0;
  for (std::int64_t tick = 0; tick < count; ++tick) {
    const std::int64_t draw = keyed_poisson(0x5eed, key, tick, mean);
    EXPECT_GE(draw, 0) << tick;
    const double off = static_cast<double>(draw) - mean;
    sum += off;
    squares += off * off;
  }
  const double shift = sum / count;
  return {mean + shift, squares / count - shift * shift};
}

TEST(KeyedPoisson, SameKeySameCount) {
  for (const double mean : {0.5, 12.0, 312.0, 1e6}) {
    const std::int64_t first = keyed_poisson(0x5eed, "CDD", 42, mean);
    // Whatever was drawn in between.
    (void)keyed_poisson(0x5eed, "EM", 42, mean);
    (void)keyed_poisson(0x5eee, "CDD", 43, mean);
    EXPECT_EQ(keyed_poisson(0x5eed, "CDD", 42, mean), first) << mean;
  }
  // Another tick, another key: other counts (not each one, but not all the same).
  int differing = 0;
  for (std::int64_t tick = 0; tick < 100; ++tick) {
    if (keyed_poisson(1, "a", tick, 312.0) != keyed_poisson(1, "a", tick + 1, 312.0)) ++differing;
    if (keyed_poisson(1, "a", tick, 312.0) != keyed_poisson(1, "b", tick, 312.0)) ++differing;
  }
  EXPECT_GT(differing, 180);
}

TEST(KeyedPoisson, ZeroOrNegativeMeanIsZero) {
  for (std::int64_t tick = 0; tick < 100; ++tick) {
    EXPECT_EQ(keyed_poisson(0x5eed, "CDD", tick, 0.0), 0);
    EXPECT_EQ(keyed_poisson(0x5eed, "CDD", tick, -0.0), 0);
    EXPECT_EQ(keyed_poisson(0x5eed, "CDD", tick, -1.0), 0);
    EXPECT_EQ(keyed_poisson(0x5eed, "CDD", tick, -1e300), 0);
    EXPECT_EQ(keyed_poisson(0x5eed, "CDD", tick, -std::numeric_limits<double>::infinity()), 0);
    EXPECT_EQ(keyed_poisson(0x5eed, "CDD", tick, std::numeric_limits<double>::quiet_NaN()), 0);
  }
}

TEST(KeyedPoisson, AMeanTooLargeToCountStaysInRange) {
  for (std::int64_t tick = 0; tick < 100; ++tick) {
    for (const double mean : {1e18, 1e19, 1e300, std::numeric_limits<double>::infinity()}) {
      const std::int64_t draw = keyed_poisson(0x5eed, "CDD", tick, mean);
      EXPECT_GE(draw, 0) << mean;
      EXPECT_LE(draw, std::int64_t{1} << 62) << mean;
    }
    // A mean next to nothing: no count, and the draw ends.
    EXPECT_EQ(keyed_poisson(0x5eed, "CDD", tick, std::numeric_limits<double>::denorm_min()), 0);
  }
}

TEST(KeyedPoisson, MeanAndVarianceMatchTheMean) {
  const int count = 100000;
  for (const double mean : {0.5, 3.0, 29.9, 30.0, 312.0, 1e4, 1e9}) {
    const Moments got = poisson_moments("CDD", mean, count);
    EXPECT_NEAR(got.mean, mean, 5.0 * std::sqrt(mean / count)) << mean;
    EXPECT_NEAR(got.variance, mean, (mean < 5.0 ? 0.05 : 0.03) * mean) << mean;
  }
}

TEST(KeyedPoisson, NoStepAtTheSwitchOfMethod) {
  const int count = 200000;
  const Moments below = poisson_moments("below", 29.99, count);
  const Moments above = poisson_moments("above", 30.01, count);
  // The means asked for differ by 0.02; what is left is the draws'.
  const double se_of_difference = std::sqrt((29.99 + 30.01) / count);
  EXPECT_NEAR((above.mean - below.mean) - 0.02, 0.0, 5.0 * se_of_difference);
  EXPECT_LT(std::abs(above.mean - below.mean), 5.0 * se_of_difference);
  EXPECT_NEAR(below.variance, 29.99, 0.03 * 29.99);
  EXPECT_NEAR(above.variance, 30.01, 0.03 * 30.01);
  EXPECT_NEAR(above.variance / below.variance, 1.0, 0.03);
}

// Counts computed once (Apple clang, libc++, arm64) and written here: a
// platform whose maths library or compiler gives another count fails.
TEST(KeyedPoisson, PinnedValues) {
  struct Case {
    std::uint64_t seed;
    std::string_view key;
    std::int64_t tick;
    double mean;
    std::int64_t count;
  };
  const Case cases[] = {
      // Multiplication (mean under 30). No product comes within a tenth of
      // exp(-mean): a last-place difference in exp changes nothing.
      {0x5eed, "CDD", 4, 0.5, 1},
      {0x5eed, "CDD", 1000000000, 3.0, 4},
      {42, "EM", -7, 12.0, 12},
      {1, "IC0", 123456789012345, 25.0, 24},
      {0x5eed, "counter1", 17, 29.9, 25},
      // Normal approximation. Each is 0.24 or more from a half-integer.
      {0x5eed, "CDD", 1, 30.0, 26},
      {0x5eed, "CDD", 1000000000, 312.0, 298},
      {42, "EM", -7, 1e4, 9960},
      {1, "IC0", 123456789012346, 1e6, 1000411},
      {0xffffffffffffffffULL, "", 3, 1e9, 1000040521},
  };
  for (const Case& c : cases) {
    EXPECT_EQ(keyed_poisson(c.seed, c.key, c.tick, c.mean), c.count)
        << "seed " << c.seed << " key " << c.key << " tick " << c.tick << " mean " << c.mean;
    if (c.mean < 30.0) continue;
    // Not a case a last-place difference in log, cos or sqrt could round
    // the other way.
    const double unrounded = c.mean + std::sqrt(c.mean) * keyed_gauss(c.seed, c.key, c.tick);
    EXPECT_GE(std::abs(unrounded - std::floor(unrounded) - 0.5), 0.01) << c.mean;
  }
}

}  // namespace
