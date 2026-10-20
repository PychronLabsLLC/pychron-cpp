// The switch access and interlock rule as pure functions, and the interlock
// topology SwitchManager hands out.

#include "pychron/systems/switch_rules.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "pychron/systems/switch_manager.hpp"

using namespace pychron;
using namespace pychron::systems;

namespace {

SwitchView view(std::string name, ValveState state = ValveState::Closed) {
  SwitchView v;
  v.name = std::move(name);
  v.state = state;
  return v;
}

class NoActuator final : public IValveActuator {
 public:
  Result<void> open(const ValveAddress&) override { return {}; }
  Result<void> close(const ValveAddress&) override { return {}; }
  Result<ValveState> read(const ValveAddress&) override { return ValveState::Closed; }
};

}  // namespace

TEST(SwitchRules, LockedRefuses) {
  auto a = view("A");
  a.locked = true;
  auto r = check_switch_access(a, "script");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(r.error().what, "'A' is software locked");
}

TEST(SwitchRules, OtherOwnerRefuses) {
  auto a = view("A");
  a.owner = "ui";
  auto r = check_switch_access(a, "script");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(r.error().what, "'A' is owned by 'ui', not 'script'");
  EXPECT_TRUE(check_switch_access(a, "ui"));
}

TEST(SwitchRules, OpenExclusivePartnerRefuses) {
  const auto a = view("A");
  const auto b = view("B", ValveState::Open);
  auto r = check_switch_interlocks(a, {&b}, {});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(r.error().what, "cannot open 'A': interlocked with 'B' which is open");
}

TEST(SwitchRules, UnknownPartnerRefuses) {
  const auto a = view("A");
  const auto b = view("B", ValveState::Unknown);
  auto r = check_switch_interlocks(a, {&b}, {});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().what, "cannot open 'A': interlocked with 'B' which is unknown");
}

TEST(SwitchRules, ClosedPositivePartnerRefuses) {
  const auto a = view("A");
  const auto b = view("B", ValveState::Closed);
  auto r = check_switch_interlocks(a, {}, {&b});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(r.error().what, "cannot open 'A': requires 'B' open, it is closed");
}

TEST(SwitchRules, AllClearIsOk) {
  const auto a = view("A");
  const auto closed = view("B");
  const auto open = view("C", ValveState::Open);
  EXPECT_TRUE(check_switch_interlocks(a, {&closed}, {&open}));
  EXPECT_TRUE(check_switch_interlocks(a, {}, {}));
}

TEST(SwitchRules, InterlockSetsAreBothWays) {
  NoActuator act;
  auto spec = [](std::string name, std::vector<std::string> neg, std::vector<std::string> pos) {
    SwitchSpec s;
    s.name = std::move(name);
    s.actuator = "act";
    s.address = ValveAddress{"1"};
    s.interlocks = std::move(neg);
    s.positive_interlocks = std::move(pos);
    return s;
  };
  auto made = SwitchManager::create({spec("A", {"B"}, {"C"}), spec("B", {}, {}), spec("C", {}, {})},
                                    [&](const std::string&) -> IValveActuator* { return &act; });
  ASSERT_TRUE(made);
  const auto sets = (*made)->interlock_sets();
  ASSERT_EQ(sets.size(), 3U);
  EXPECT_EQ(sets.find("A")->second.exclusive, std::vector<std::string>{"B"});
  EXPECT_EQ(sets.find("A")->second.positive, std::vector<std::string>{"C"});
  EXPECT_EQ(sets.find("B")->second.exclusive, std::vector<std::string>{"A"});
  EXPECT_TRUE(sets.find("B")->second.positive.empty());
  EXPECT_TRUE(sets.find("C")->second.exclusive.empty());
}
