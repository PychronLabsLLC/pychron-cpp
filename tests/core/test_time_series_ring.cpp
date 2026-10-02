#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "pychron/core/time_series_ring.hpp"

using namespace pychron;

TEST(TimeSeriesRing, KeepsPointsWithinSpan) {
  TimeSeriesRing ring(10.0);
  for (int t = 0; t <= 20; ++t) ring.push(t, t);
  EXPECT_EQ(ring.size(), 11U);
  EXPECT_DOUBLE_EQ(ring[0].t, 10.0);
  EXPECT_DOUBLE_EQ(ring.back().t, 20.0);
}

TEST(TimeSeriesRing, SetSpanShrinkTrimsImmediately) {
  TimeSeriesRing ring(100.0);
  for (int t = 0; t <= 20; ++t) ring.push(t, t);
  EXPECT_EQ(ring.size(), 21U);
  ring.set_span(5.0);
  EXPECT_DOUBLE_EQ(ring.span(), 5.0);
  EXPECT_EQ(ring.size(), 6U);
  EXPECT_DOUBLE_EQ(ring[0].t, 15.0);
}

TEST(TimeSeriesRing, OutOfOrderPushIgnored) {
  TimeSeriesRing ring(10.0);
  ring.push(5.0, 1.0);
  ring.push(3.0, 2.0);
  EXPECT_EQ(ring.size(), 1U);
  EXPECT_DOUBLE_EQ(ring.back().value, 1.0);
}

TEST(TimeSeriesRing, EqualTimestampsAccepted) {
  TimeSeriesRing ring(10.0);
  ring.push(5.0, 1.0);
  ring.push(5.0, 2.0);
  EXPECT_EQ(ring.size(), 2U);
  EXPECT_DOUBLE_EQ(ring.back().value, 2.0);
}

TEST(TimeSeriesRing, RangeOverSubWindow) {
  TimeSeriesRing ring(100.0);
  for (int t = 1; t <= 10; ++t) ring.push(t, t);
  const auto r = ring.range(3.0, 5.0);
  ASSERT_TRUE(r.has_value());
  EXPECT_DOUBLE_EQ(r->first, 3.0);
  EXPECT_DOUBLE_EQ(r->second, 5.0);
}

TEST(TimeSeriesRing, RangeSkipsNaNAndEmptyIsNullopt) {
  TimeSeriesRing ring(100.0);
  EXPECT_FALSE(ring.range(0.0, 10.0).has_value());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  ring.push(1.0, nan);
  EXPECT_FALSE(ring.range(0.0, 10.0).has_value());
  ring.push(2.0, 4.0);
  ring.push(3.0, nan);
  ring.push(4.0, -2.0);
  const auto r = ring.range(0.0, 10.0);
  ASSERT_TRUE(r.has_value());
  EXPECT_DOUBLE_EQ(r->first, -2.0);
  EXPECT_DOUBLE_EQ(r->second, 4.0);
  EXPECT_FALSE(ring.range(20.0, 30.0).has_value());
}

TEST(TimeSeriesRing, HardCapDropsOldest) {
  TimeSeriesRing ring(1e12);
  for (std::size_t i = 0; i < TimeSeriesRing::kMaxPoints + 5; ++i) {
    ring.push(static_cast<double>(i), 0.0);
  }
  EXPECT_EQ(ring.size(), TimeSeriesRing::kMaxPoints);
  EXPECT_DOUBLE_EQ(ring[0].t, 5.0);
}

TEST(TimeSeriesRing, ClearEmpties) {
  TimeSeriesRing ring(10.0);
  ring.push(1.0, 1.0);
  ring.clear();
  EXPECT_TRUE(ring.empty());
  EXPECT_EQ(ring.size(), 0U);
}

TEST(TimeSeriesRing, NonPositiveSpanClampsToSmallPositive) {
  for (const double span : {0.0, -5.0}) {
    TimeSeriesRing ring(span);
    EXPECT_GT(ring.span(), 0.0);
    ring.push(1.0, 1.0);
    ring.push(2.0, 2.0);
    ring.push(3.0, 3.0);
    EXPECT_EQ(ring.size(), 1U);
    EXPECT_DOUBLE_EQ(ring.back().t, 3.0);
  }
}
