#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <future>

#include "jobs_fakes.hpp"
#include "pychron/systems/jobs/sweep.hpp"

using namespace pychron;
using namespace pychron::jobs;
using namespace pychron::spectrometer::testing;
using pychron::spectrometer::ChannelId;
using namespace std::chrono_literals;

namespace {

SweepSpec magnet_sweep(double start, double stop, double step) {
  SweepSpec s;
  s.axis = SweepAxis::magnet();
  s.start = start;
  s.stop = stop;
  s.step = step;
  return s;
}

std::vector<double> xs(const std::vector<SweepPoint>& points) {
  std::vector<double> out;
  for (const auto& p : points) out.push_back(p.x);
  return out;
}

void expect_near(const std::vector<double>& got, const std::vector<double>& want) {
  ASSERT_EQ(got.size(), want.size());
  for (std::size_t i = 0; i < got.size(); ++i) EXPECT_NEAR(got[i], want[i], 1e-9) << "index " << i;
}

}  // namespace

// ---- sweep_positions ------------------------------------------------------

TEST(SweepPositions, AscendingIncludesStop) {
  auto p = sweep_positions(magnet_sweep(1.0, 2.0, 0.25));
  ASSERT_TRUE(p.has_value());
  expect_near(*p, {1.0, 1.25, 1.5, 1.75, 2.0});
}

TEST(SweepPositions, DescendingTakesDirectionFromStartStop) {
  auto p = sweep_positions(magnet_sweep(2.0, 1.0, -0.5));
  ASSERT_TRUE(p.has_value());
  expect_near(*p, {2.0, 1.5, 1.0});
  auto q = sweep_positions(magnet_sweep(2.0, 1.0, 0.5));
  ASSERT_TRUE(q.has_value());
  expect_near(*q, {2.0, 1.5, 1.0});
}

TEST(SweepPositions, StopNotOnGridIsNotOvershot) {
  auto p = sweep_positions(magnet_sweep(0.0, 1.0, 0.4));
  ASSERT_TRUE(p.has_value());
  expect_near(*p, {0.0, 0.4, 0.8});
}

TEST(SweepPositions, BidirectionalReturnsWithoutRepeatingTurnPoint) {
  auto s = magnet_sweep(0.0, 1.0, 0.5);
  s.bidirectional = true;
  auto p = sweep_positions(s);
  ASSERT_TRUE(p.has_value());
  expect_near(*p, {0.0, 0.5, 1.0, 0.5, 0.0});
}

TEST(SweepPositions, SinglePointWhenStartEqualsStop) {
  auto p = sweep_positions(magnet_sweep(3.0, 3.0, 0.1));
  ASSERT_TRUE(p.has_value());
  expect_near(*p, {3.0});
}

TEST(SweepPositions, RejectsBadSpecs) {
  for (auto s : {magnet_sweep(0, 1, 0.0), magnet_sweep(0, 1, NAN), magnet_sweep(0, INFINITY, 0.1),
                 magnet_sweep(0, 1e9, 1e-9)}) {
    auto p = sweep_positions(s);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error().kind, ErrorKind::Config);
  }
}

// ---- Sweep::run -----------------------------------------------------------

TEST(Sweep, MagnetSweepRecordsSignalAtEachPosition) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  // Gaussian peak on H1 centered at DAC 5.0; AX flat.
  r.acquirer.signal = [&](const ChannelId& ch) {
    const double x = r.positioner.value;
    if (ch == "H1") return 1000.0 * std::exp(-(x - 5.0) * (x - 5.0) / 0.1);
    return ch == "AX" ? 7.0 : 0.0;
  };
  SweepSpec s = magnet_sweep(4.0, 6.0, 0.5);
  s.settle = 50ms;
  s.record = {"H1", "AX"};
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_TRUE(result.has_value()) << to_string(result.error());

  expect_near(xs(*result), {4.0, 4.5, 5.0, 5.5, 6.0});
  expect_near(r.positioner.sets, {4.0, 4.5, 5.0, 5.5, 6.0});
  std::size_t best = 0;
  for (std::size_t i = 0; i < result->size(); ++i) {
    const auto& y = (*result)[i].y;
    EXPECT_EQ(y.size(), 2U);
    EXPECT_NEAR(y.at("AX"), 7.0, 1e-9);
    if (y.at("H1") > (*result)[best].y.at("H1")) best = i;
  }
  EXPECT_EQ(best, 2U);
  EXPECT_NEAR((*result)[2].y.at("H1"), 1000.0, 1e-6);
  EXPECT_EQ(sweep.points(), *result);
  // Settle after every set.
  EXPECT_EQ(r.sleeps, std::vector<Duration>(5, 50ms));
  for (std::size_t i = 1; i < result->size(); ++i) EXPECT_GT((*result)[i].ts, (*result)[i - 1].ts);
}

TEST(Sweep, EmptyRecordKeepsEveryDetector) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [](const ChannelId&) { return 1.0; };
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto s = magnet_sweep(1.0, 1.0, 0.1);
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_TRUE(result.has_value()) << to_string(result.error());
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(result->front().y.size(), 6U);
}

TEST(Sweep, UnknownRecordDetectorIsConfigError) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  Sweep sweep;
  Progress progress;
  CancelToken cancel;
  auto s = magnet_sweep(1.0, 2.0, 0.5);
  s.record = {"H9"};
  auto result = sweep.run(*r.spec, s, progress, cancel);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::Config);
  EXPECT_TRUE(r.positioner.sets.empty());
}

TEST(Sweep, HvAxisDrivesSource) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [&](const ChannelId&) { return r.source.hv / 1000.0; };
  SweepSpec s;
  s.axis = SweepAxis::hv();
  s.start = 4400;
  s.stop = 4600;
  s.step = 100;
  s.record = {"H1"};
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_TRUE(result.has_value()) << to_string(result.error());
  expect_near(xs(*result), {4400, 4500, 4600});
  EXPECT_NEAR(result->back().y.at("H1"), 4.6, 1e-9);
  EXPECT_DOUBLE_EQ(r.source.hv, 4600);
  EXPECT_TRUE(r.positioner.sets.empty());
}

TEST(Sweep, SourceParamAxisDrivesParam) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [&](const ChannelId&) { return r.source.trap; };
  SweepSpec s;
  s.axis = SweepAxis::source_param(spectrometer::SourceParam::TrapCurrent);
  s.start = 100;
  s.stop = 200;
  s.step = 50;
  s.record = {"H1"};
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_TRUE(result.has_value()) << to_string(result.error());
  ASSERT_EQ(result->size(), 3U);
  EXPECT_NEAR((*result)[1].y.at("H1"), 150.0, 1e-9);
  EXPECT_DOUBLE_EQ(r.source.trap, 200);
}

TEST(Sweep, CddVoltageAxisDrivesDetectorControl) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [](const ChannelId&) { return 10.0; };
  SweepSpec s;
  s.axis = SweepAxis::cdd_voltage("CDD");
  s.start = 1400;
  s.stop = 1500;
  s.step = 50;
  s.record = {"CDD"};
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_TRUE(result.has_value()) << to_string(result.error());
  expect_near(xs(*result), {1400, 1450, 1500});
  ASSERT_EQ(r.control.cdd.size(), 1U);
  EXPECT_DOUBLE_EQ(r.control.cdd.begin()->second, 1500);
  EXPECT_EQ(r.spec->detector_state("CDD")->cdd_voltage, 1500);
}

TEST(Sweep, DeflectionAxisRecordsClampedValue) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [](const ChannelId&) { return 1.0; };
  SweepSpec s;
  s.axis = SweepAxis::deflection("H1");
  s.start = 700;
  s.stop = 900;
  s.step = 100;  // max = 800
  s.record = {"H1"};
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_TRUE(result.has_value()) << to_string(result.error());
  expect_near(xs(*result), {700, 800, 800});
}

TEST(Sweep, BidirectionalVisitsBothDirections) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [](const ChannelId&) { return 1.0; };
  auto s = magnet_sweep(1.0, 2.0, 0.5);
  s.bidirectional = true;
  s.record = {"H1"};
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_TRUE(result.has_value()) << to_string(result.error());
  expect_near(xs(*result), {1.0, 1.5, 2.0, 1.5, 1.0});
  expect_near(r.positioner.sets, {1.0, 1.5, 2.0, 1.5, 1.0});
}

TEST(Sweep, ReportsProgressWithEachPoint) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [&](const ChannelId&) { return r.positioner.value; };
  auto s = magnet_sweep(1.0, 2.0, 0.5);
  s.record = {"H1"};
  std::vector<ProgressUpdate> updates;
  Progress progress([&](const ProgressUpdate& u) { updates.push_back(u); });
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_TRUE(result.has_value()) << to_string(result.error());
  std::vector<ProgressUpdate> with_points;
  for (const auto& u : updates) {
    if (u.point) with_points.push_back(u);
  }
  ASSERT_EQ(with_points.size(), 3U);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(with_points[i].done, i + 1);
    EXPECT_EQ(with_points[i].total, 3U);
    EXPECT_EQ(*with_points[i].point, (*result)[i]);
  }
  EXPECT_DOUBLE_EQ(progress.last().fraction(), 1.0);
}

TEST(Sweep, CancelStopsBetweenPointsAndKeepsPoints) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [](const ChannelId&) { return 1.0; };
  auto s = magnet_sweep(0.0, 5.0, 1.0);
  s.record = {"H1"};
  CancelToken cancel;
  Progress progress([&](const ProgressUpdate& u) {
    if (u.point && u.done == 2) cancel.cancel();
  });
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
  EXPECT_EQ(sweep.points().size(), 2U);
  EXPECT_EQ(r.positioner.sets.size(), 2U);
}

TEST(Sweep, CancelInterruptsBlockedAcquisition) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  // No frames ever arrive: acquire() blocks until cancelled.
  r.acquirer.signal = [](const ChannelId&) { return 1.0; };
  auto s = magnet_sweep(0.0, 1.0, 1.0);
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  // Never poll the engine, so the first reading never completes.
  while (fut.wait_for(5ms) != std::future_status::ready) cancel.cancel();
  auto result = fut.get();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
  EXPECT_TRUE(sweep.points().empty());
}

TEST(Sweep, IntegrationIsAppliedAndEngineLeftStopped) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [](const ChannelId&) { return 1.0; };
  auto s = magnet_sweep(1.0, 2.0, 1.0);
  s.integration = 250ms;
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_TRUE(result.has_value()) << to_string(result.error());
  ASSERT_FALSE(r.acquirer.configured.empty());
  EXPECT_EQ(r.acquirer.configured.front(), 250ms);
  EXPECT_FALSE(r.spec->acquisition().running());
}

TEST(Sweep, HardwareErrorAbortsSweep) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.acquirer.signal = [](const ChannelId&) { return 1.0; };
  r.positioner.fail_set_at = 2;
  auto s = magnet_sweep(1.0, 3.0, 1.0);
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Never});
  Progress progress;
  CancelToken cancel;
  auto fut = std::async(std::launch::async, [&] { return sweep.run(*r.spec, s, progress, cancel); });
  auto result = r.drive(fut);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::Io);
  EXPECT_EQ(sweep.points().size(), 1U);
}

// A sweep step moves with a zero settle (the sweep settles itself). When the
// step fails after the set went out, the facade still holds the protection and
// the blank for [magnet].settle_ms (500 in the example config) before cleanup.
TEST(Sweep, FailedMagnetStepWaitsTheConfiguredSettleBeforeCleanup) {
  JobRig r;
  ASSERT_TRUE(r.spec);
  r.positioner.timeout_set_at = 1;  // written, then no reply
  auto s = magnet_sweep(5.0, 6.0, 0.5);
  s.settle = 50ms;
  Sweep sweep(Sweep::Options{r.sweep_sleep(), spectrometer::ProtectPolicy::Auto});
  Progress progress;
  CancelToken cancel;
  const TimePoint start = r.clock.now();
  auto result = sweep.run(*r.spec, s, progress, cancel);  // fails before any acquisition: no driving needed
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(r.log, (CallLog{"protect:CDD", "blank", "set:5.000", "unblank", "unprotect:CDD"}));
  EXPECT_EQ(r.clock.now() - start, 500ms);  // the facade's wait; the sweep's own settle never ran
  EXPECT_TRUE(r.sleeps.empty());
  EXPECT_TRUE(sweep.points().empty());
}
