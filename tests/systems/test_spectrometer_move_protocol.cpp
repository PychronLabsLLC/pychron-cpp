#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <random>

#include "pychron/systems/spectrometer/move_protocol.hpp"
#include "spectrometer_fakes.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace pychron::spectrometer::testing;
using namespace std::chrono_literals;

namespace {

struct Rig {
  CallLog log;
  ManualClock clock;
  FakePositioner positioner{log};
  FakeControl control{log};
  FakeBlank blank{log};
  std::vector<Duration> sleeps;
  std::vector<std::pair<ChannelId, bool>> recorded;

  MoveDeps deps() {
    return MoveDeps{positioner, &control, &blank, clock,
                    [this](Duration d) {
                      sleeps.push_back(d);
                      clock.advance(d);
                    },
                    [this](const ChannelId& ch, bool on) { recorded.emplace_back(ch, on); }};
  }
};

MovePlan plan(double from, double to) {
  MovePlan p;
  p.from = from;
  p.to = to;
  p.settle = 500ms;
  p.poll_interval = 50ms;
  p.max_wait = 1s;
  return p;
}

}  // namespace

TEST(MoveProtocol, ProtectBlankSetThenUnblankUnprotectInReverse) {
  Rig r;
  auto p = plan(4.0, 5.0);
  p.protect = {"CDD", "H1"};
  p.blank = true;
  auto out = execute_move(p, r.deps());
  ASSERT_TRUE(out.has_value()) << out.error().what;
  EXPECT_EQ(r.log, (CallLog{"protect:CDD", "protect:H1", "blank", "set:5.000", "unblank", "unprotect:H1", "unprotect:CDD"}));
  EXPECT_EQ(out->protected_channels, (std::vector<ChannelId>{"CDD", "H1"}));
  EXPECT_TRUE(out->blanked);
  EXPECT_FALSE(r.control.any_protected());
  EXPECT_EQ(r.recorded.size(), 4U);
  EXPECT_EQ(r.recorded.back(), (std::pair<ChannelId, bool>{"CDD", false}));
}

TEST(MoveProtocol, SetFailureStillUnprotectsAndReturnsFirstError) {
  Rig r;
  r.positioner.fail_set_at = 1;
  r.blank.fail_off = true;  // a later cleanup error must not mask the first
  auto p = plan(4.0, 5.0);
  p.protect = {"CDD"};
  p.blank = true;
  auto out = execute_move(p, r.deps());
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().what, "set failed");
  EXPECT_EQ(r.log, (CallLog{"protect:CDD", "blank", "set-fail:5.000", "unblank-fail", "unprotect:CDD"}));
  EXPECT_FALSE(r.control.any_protected());
}

TEST(MoveProtocol, ProtectFailureMidwayUnprotectsEverythingAttempted) {
  Rig r;
  r.control.fail_protect = {"H1"};
  auto p = plan(4.0, 5.0);
  p.protect = {"CDD", "H1", "L1"};
  auto out = execute_move(p, r.deps());
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(r.log, (CallLog{"protect:CDD", "protect-fail:H1", "unprotect:H1", "unprotect:CDD"}));
  EXPECT_TRUE(r.positioner.sets.empty());
  EXPECT_FALSE(r.control.any_protected());
}

TEST(MoveProtocol, ProtectionWithoutDetectorControlIsConfigError) {
  Rig r;
  auto deps = r.deps();
  deps.control = nullptr;
  auto p = plan(4.0, 5.0);
  p.protect = {"CDD"};
  auto out = execute_move(p, deps);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, ErrorKind::Config);
  EXPECT_TRUE(r.positioner.sets.empty());
}

TEST(MoveProtocol, SettlesWhenPositionerNeverReportsMotion) {
  Rig r;
  auto out = execute_move(plan(4.0, 5.0), r.deps());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(r.sleeps, (std::vector<Duration>{500ms}));
  EXPECT_EQ(out->elapsed, 500ms);
}

TEST(MoveProtocol, SettleSkippedForTinyMoves) {
  Rig r;
  auto out = execute_move(plan(5.0, 5.0 + 1e-9), r.deps());
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(r.sleeps.empty());
}

TEST(MoveProtocol, PollsMovingInsteadOfSettle) {
  Rig r;
  r.positioner.moving_polls = 3;
  auto out = execute_move(plan(4.0, 5.0), r.deps());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(r.sleeps, (std::vector<Duration>{50ms, 50ms, 50ms}));
  EXPECT_EQ(r.positioner.moving_calls, 4);
}

TEST(MoveProtocol, MovingBeyondMaxWaitTimesOutAndUnprotects) {
  Rig r;
  r.positioner.moving_polls = 1000;
  auto p = plan(4.0, 5.0);
  p.protect = {"CDD"};
  auto out = execute_move(p, r.deps());
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().kind, ErrorKind::Timeout);
  EXPECT_FALSE(r.control.any_protected());
  EXPECT_EQ(r.log.back(), "unprotect:CDD");
}

TEST(MoveProtocol, AfDemagTrajectoryDecaysAroundTarget) {
  AfDemagSettings af{true, 500ms, 2s, 0.4, 0.5};
  auto traj = af_demag_trajectory(3.0, 5.0, af, Limits{0.0, 10.0});
  ASSERT_EQ(traj.size(), static_cast<std::size_t>(4 * kAfDemagStepsPerPeriod));
  EXPECT_DOUBLE_EQ(traj[0], 5.0);                  // sin(0)
  EXPECT_NEAR(traj[2], 5.0 + 0.4 * (1.0 - 0.125 / 2.0), 1e-12);  // quarter period peak
  for (std::size_t i = 0; i < traj.size(); ++i) {
    const double t = 0.0625 * static_cast<double>(i);
    EXPECT_LE(std::abs(traj[i] - 5.0), 0.4 * (1.0 - t / 2.0) + 1e-12);
  }
  EXPECT_TRUE(af_demag_trajectory(4.8, 5.0, af, Limits{0.0, 10.0}).empty());  // below threshold
  af.enabled = false;
  EXPECT_TRUE(af_demag_trajectory(3.0, 5.0, af, Limits{0.0, 10.0}).empty());
}

TEST(MoveProtocol, AfDemagClampsToLimits) {
  AfDemagSettings af{true, 400ms, 400ms, 2.0, 0.1};
  for (double v : af_demag_trajectory(0.0, 9.5, af, Limits{0.0, 10.0})) EXPECT_LE(v, 10.0);
}

TEST(MoveProtocol, AfDemagSetsRunBeforeFinalSet) {
  Rig r;
  auto p = plan(3.0, 5.0);
  p.af_demag = AfDemagSettings{true, 400ms, 400ms, 0.5, 0.5};
  auto out = execute_move(p, r.deps());
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->demag.size(), static_cast<std::size_t>(kAfDemagStepsPerPeriod));
  ASSERT_EQ(r.positioner.sets.size(), out->demag.size() + 1);
  EXPECT_DOUBLE_EQ(r.positioner.sets.back(), 5.0);
  for (std::size_t i = 0; i < out->demag.size(); ++i) EXPECT_DOUBLE_EQ(r.positioner.sets[i], out->demag[i]);
}

TEST(MoveProtocol, AfDemagClampsToPlanLimitsWhenSet) {
  Rig r;  // positioner limits 0..10
  auto p = plan(3.0, 5.9);
  p.af_demag = AfDemagSettings{true, 400ms, 400ms, 0.5, 0.5};
  p.limits = Limits{0.0, 6.0};
  auto out = execute_move(p, r.deps());
  ASSERT_TRUE(out.has_value());
  ASSERT_FALSE(r.positioner.sets.empty());
  EXPECT_DOUBLE_EQ(*std::max_element(r.positioner.sets.begin(), r.positioner.sets.end()), 6.0);  // it swung past
  for (double v : r.positioner.sets) EXPECT_LE(v, 6.0);
}

// Property: random protect/blank/failure combinations never leave a detector
// protected or the beam blanked, as long as the unprotect calls themselves
// succeed.
TEST(MoveProtocol, RandomFailuresNeverLeaveDetectorsProtected) {
  std::mt19937 rng(7);
  const std::vector<ChannelId> all{"H2", "H1", "AX", "L1", "CDD"};
  for (int trial = 0; trial < 300; ++trial) {
    Rig r;
    auto p = plan(std::uniform_real_distribution<double>(0, 10)(rng), std::uniform_real_distribution<double>(0, 10)(rng));
    for (const auto& ch : all) {
      if (rng() % 2) p.protect.push_back(ch);
      if (rng() % 6 == 0) r.control.fail_protect.insert(ch);
    }
    p.blank = rng() % 2;
    r.blank.fail_on = rng() % 5 == 0;
    if (rng() % 4 == 0) r.positioner.fail_set_at = 1;
    if (rng() % 4 == 0) r.positioner.moving_polls = 1000;
    auto out = execute_move(p, r.deps());
    EXPECT_FALSE(r.control.any_protected()) << "trial " << trial;
    EXPECT_FALSE(r.blank.blanked) << "trial " << trial;
    (void)out;
  }
}
