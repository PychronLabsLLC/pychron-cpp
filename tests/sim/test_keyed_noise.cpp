#include "pychron/sim/keyed_noise.hpp"

#include <cmath>
#include <cstdint>
#include <set>

#include <gtest/gtest.h>

namespace {

using pychron::sim::keyed_bits;
using pychron::sim::keyed_gauss;

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

}  // namespace
