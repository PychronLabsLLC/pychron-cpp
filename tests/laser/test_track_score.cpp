#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "pychron/laser/track_score.hpp"

using pychron::laser::summarize;
using pychron::laser::TrackScore;
using pychron::laser::TrackScorer;
using pychron::laser::TrackSummary;
using Reason = pychron::vision::DragonflyStep::Reason;

namespace {
constexpr double kRadius = 1.0;
constexpr double kOn = 0.0;
constexpr double kOff = 5.0;

// Samples every 0.1 s from `from` up to (not including) `to`.
void run(TrackScorer& s, double from, double to, double dist, double heat = 1.0) {
  const int a = static_cast<int>(std::lround(from * 10)), b = static_cast<int>(std::lround(to * 10));
  for (int i = a; i < b; ++i) s.sample(i / 10.0, dist, heat);
}
}  // namespace

TEST(TrackScorer, AlwaysOn) {
  TrackScorer s(kRadius, 1.0, 30);
  run(s, 0, 30, kOn);
  const TrackScore r = s.finish(30);
  EXPECT_DOUBLE_EQ(r.on_pct, 100.0);
  EXPECT_NEAR(r.lock_s, 1.0, 1e-9);
  EXPECT_DOUBLE_EQ(r.lost, 0);
  EXPECT_DOUBLE_EQ(r.back_s, 0.0);
}

TEST(TrackScorer, NeverOn) {
  TrackScorer s(kRadius, 1.0, 30);
  run(s, 0, 30, kOff);
  const TrackScore r = s.finish(30);
  EXPECT_DOUBLE_EQ(r.on_pct, 0.0);
  EXPECT_DOUBLE_EQ(r.lock_s, 30.0);
  EXPECT_DOUBLE_EQ(r.lost, 0);
  EXPECT_NEAR(r.rms_mm, kOff, 1e-9);
  EXPECT_DOUBLE_EQ(r.max_mm, kOff);
}

TEST(TrackScorer, ABriefMissIsNoLoss) {
  TrackScorer s(kRadius, 1.0, 30);
  run(s, 0, 10, kOn);
  run(s, 10, 10.5, kOff);
  run(s, 10.5, 30, kOn);
  const TrackScore r = s.finish(30);
  EXPECT_DOUBLE_EQ(r.lost, 0);
  EXPECT_NEAR(r.lock_s, 1.0, 1e-9);
  EXPECT_DOUBLE_EQ(r.back_s, 0.0);
}

TEST(TrackScorer, ALossIsDatedFromGoingOff) {
  TrackScorer s(kRadius, 1.0, 30);
  run(s, 0, 10, kOn);
  run(s, 10, 13, kOff);
  run(s, 13, 30, kOn);
  const TrackScore r = s.finish(30);
  EXPECT_DOUBLE_EQ(r.lost, 1);
  EXPECT_NEAR(r.back_s, 4.0, 1e-9);  // off at 10, on again at 13, locked at 14
}

TEST(TrackScorer, NeverBackCountsTheRest) {
  TrackScorer s(kRadius, 1.0, 30);
  run(s, 0, 10, kOn);
  run(s, 10, 30, kOff);
  const TrackScore r = s.finish(30);
  EXPECT_DOUBLE_EQ(r.lost, 1);
  EXPECT_NEAR(r.back_s, 20.0, 1e-9);
}

TEST(TrackScorer, ReasonsAreBinned) {
  TrackScorer s(kRadius, 1.0, 30);
  s.look(Reason::Track);
  s.look(Reason::PerimeterClamp);
  s.look(Reason::Deadband);
  s.look(Reason::Miss);
  s.look(Reason::Saturated);
  s.look(Reason::Search);
  s.look(Reason::Elapsed);
  s.look(Reason::Invalid);
  s.moved();
  s.moved();
  const TrackScore r = s.finish(30);
  EXPECT_NEAR(r.track_pct, 100.0 / 3, 1e-9);
  EXPECT_NEAR(r.hold_pct, 100.0 / 3, 1e-9);
  EXPECT_NEAR(r.sat_pct, 100.0 / 6, 1e-9);
  EXPECT_NEAR(r.search_pct, 100.0 / 6, 1e-9);
  EXPECT_NEAR(r.track_pct + r.hold_pct + r.sat_pct + r.search_pct, 100.0, 1e-9);
  EXPECT_DOUBLE_EQ(r.moves, 2);
}

TEST(TrackScorer, HeatIsAgainstFullHeat) {
  TrackScorer s(kRadius, 1.0, 30);
  run(s, 0, 30, kOn, 0.5);
  EXPECT_NEAR(s.finish(30).heat_pct, 50.0, 1e-9);
}

TEST(TrackScorer, ThereAreNoLooks) {
  TrackScorer s(kRadius, 1.0, 30);
  run(s, 0, 5, kOn);
  const TrackScore r = s.finish(5);
  EXPECT_DOUBLE_EQ(r.track_pct, 0.0);
  EXPECT_DOUBLE_EQ(r.hold_pct, 0.0);
  EXPECT_DOUBLE_EQ(r.sat_pct, 0.0);
  EXPECT_DOUBLE_EQ(r.search_pct, 0.0);
}

TEST(TrackSummary, MeanAndWorst) {
  TrackScore a, b;
  a.on_pct = 90;
  a.heat_pct = 80;
  a.lost = 1;
  a.rms_mm = 0.2;
  a.moves = 4;
  b.on_pct = 50;
  b.heat_pct = 60;
  b.lost = 3;
  b.rms_mm = 0.6;
  b.moves = 8;
  const std::array runs{a, b};
  const TrackSummary s = summarize(runs);
  EXPECT_DOUBLE_EQ(s.mean.on_pct, 70.0);
  EXPECT_DOUBLE_EQ(s.mean.heat_pct, 70.0);
  EXPECT_DOUBLE_EQ(s.mean.lost, 2);
  EXPECT_DOUBLE_EQ(s.mean.moves, 6);
  EXPECT_DOUBLE_EQ(s.worst.on_pct, 50.0);
  EXPECT_DOUBLE_EQ(s.worst.heat_pct, 60.0);
  EXPECT_DOUBLE_EQ(s.worst.lost, 3);
  EXPECT_DOUBLE_EQ(s.worst.rms_mm, 0.6);
  EXPECT_DOUBLE_EQ(s.worst.moves, 8);
  EXPECT_DOUBLE_EQ(summarize({}).worst.lost, 0);
  TrackScore none, one;
  one.lost = 1;
  const std::array pair{none, one};
  EXPECT_DOUBLE_EQ(summarize(pair).mean.lost, 0.5);
}

TEST(TrackScorer, TheGrainsRadiusCountsAsOn) {
  // beam radius 0.4, beam 0.7 from the center: off a point grain, on one of radius 0.5
  TrackScorer big(0.4, 1.0, 30), point(0.4, 1.0, 30);
  for (int i = 0; i < 100; ++i) {
    big.sample(i / 10.0, 0.7, 1.0, 0.5);
    point.sample(i / 10.0, 0.7, 1.0);
  }
  EXPECT_DOUBLE_EQ(big.finish(10).on_pct, 100.0);
  EXPECT_DOUBLE_EQ(point.finish(10).on_pct, 0.0);
}
