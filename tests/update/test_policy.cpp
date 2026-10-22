#include <gtest/gtest.h>

#include <chrono>

#include <pychron/update/update.hpp>

namespace pychron::update {
namespace {

using namespace std::chrono_literals;

const auto kNow = std::chrono::system_clock::time_point{std::chrono::sys_days{std::chrono::year{2026} / 10 / 21}};

Release release(Version v) {
  return Release{v, "v" + to_string(v), "https://example.test/r", "notes"};
}

TEST(UpdateDue, DisabledNeverDue) {
  CheckPolicy policy;
  policy.enabled = false;
  EXPECT_FALSE(due(policy, kNow));
}

TEST(UpdateDue, NeverCheckedIsDue) {
  EXPECT_TRUE(due(CheckPolicy{}, kNow));
}

TEST(UpdateDue, JustUnderADayIsNot) {
  CheckPolicy policy;
  policy.last_check = kNow - 24h + 1s;
  EXPECT_FALSE(due(policy, kNow));
}

TEST(UpdateDue, ExactlyADayIs) {
  CheckPolicy policy;
  policy.last_check = kNow - 24h;
  EXPECT_TRUE(due(policy, kNow));
}

TEST(UpdateDue, FutureLastCheckIsDue) {
  CheckPolicy policy;
  policy.last_check = kNow + 1h;
  EXPECT_TRUE(due(policy, kNow));
}

TEST(UpdateWorthShowing, Newer) {
  const auto shown = worth_showing(release({0, 5, 0}), {0, 4, 0}, CheckPolicy{});
  ASSERT_TRUE(shown.has_value());
  EXPECT_EQ(shown->version, (Version{0, 5, 0}));
}

TEST(UpdateWorthShowing, EqualOrOlder) {
  EXPECT_FALSE(worth_showing(release({0, 4, 0}), {0, 4, 0}, CheckPolicy{}).has_value());
  EXPECT_FALSE(worth_showing(release({0, 3, 9}), {0, 4, 0}, CheckPolicy{}).has_value());
}

TEST(UpdateWorthShowing, SkippedIsHidden) {
  CheckPolicy policy;
  policy.skipped = Version{0, 5, 0};
  EXPECT_FALSE(worth_showing(release({0, 5, 0}), {0, 4, 0}, policy).has_value());
}

TEST(UpdateWorthShowing, SkippedOlderDoesNotHide) {
  CheckPolicy policy;
  policy.skipped = Version{0, 5, 0};
  EXPECT_TRUE(worth_showing(release({0, 6, 0}), {0, 4, 0}, policy).has_value());
}

TEST(UpdateWorthShowing, DisabledStillAnswers) {
  CheckPolicy policy;
  policy.enabled = false;
  EXPECT_TRUE(worth_showing(release({0, 5, 0}), {0, 4, 0}, policy).has_value());
}

}  // namespace
}  // namespace pychron::update
