// Property-style interlock tests: random networks and random open/close
// sequences (seeded, reproducible) never violate declared interlocks.

#include <gtest/gtest.h>

#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "pychron/systems/switch_manager.hpp"

using namespace pychron;
using namespace pychron::systems;

namespace {

// Ideal hardware: every command lands, reads report the truth.
class IdealActuator final : public IValveActuator {
 public:
  Result<void> open(const ValveAddress& a) override { return set(a, ValveState::Open); }
  Result<void> close(const ValveAddress& a) override { return set(a, ValveState::Closed); }
  Result<ValveState> read(const ValveAddress& a) override {
    std::lock_guard lk(m_);
    auto it = hw_.find(a.value);
    return it == hw_.end() ? ValveState::Closed : it->second;
  }

 private:
  Result<void> set(const ValveAddress& a, ValveState s) {
    std::lock_guard lk(m_);
    hw_[a.value] = s;
    return {};
  }
  std::mutex m_;
  std::map<std::string, ValveState> hw_;
};

struct Network {
  std::vector<SwitchSpec> specs;
};

// n valves V0..Vn-1. Negative interlocks between random pairs; positive
// interlocks only point to lower indices so they are acyclic, and never to a
// valve already listed as a negative interlock.
Network random_network(std::mt19937& rng, int n) {
  Network net;
  std::bernoulli_distribution neg(0.2), pos(0.15);
  for (int i = 0; i < n; ++i) {
    SwitchSpec s;
    s.name = "V" + std::to_string(i);
    s.actuator = "act";
    s.address = ValveAddress{std::to_string(i)};
    for (int j = 0; j < n; ++j) {
      if (j == i) continue;
      const auto other = "V" + std::to_string(j);
      if (neg(rng)) {
        s.interlocks.push_back(other);
      } else if (j < i && pos(rng)) {
        s.positive_interlocks.push_back(other);
      }
    }
    net.specs.push_back(std::move(s));
  }
  return net;
}

bool is_open(const std::map<std::string, ValveState>& s, const std::string& v) { return s.at(v) == ValveState::Open; }

}  // namespace

TEST(SwitchManagerProperty, RandomSequencesNeverViolateInterlocks) {
  constexpr int kNetworks = 40;
  constexpr int kSteps = 300;
  std::size_t accepted_opens = 0, refused = 0;

  for (int seed = 0; seed < kNetworks; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    std::uniform_int_distribution<int> size_dist(2, 10);
    const int n = size_dist(rng);
    auto net = random_network(rng, n);
    IdealActuator act;
    auto made = SwitchManager::create(net.specs, [&](const std::string&) -> IValveActuator* { return &act; });
    ASSERT_TRUE(made) << "seed " << seed << ": " << made.error().what;
    auto& mgr = **made;
    ASSERT_TRUE(mgr.refresh());

    std::uniform_int_distribution<int> pick(0, n - 1);
    std::bernoulli_distribution want_open(0.6);
    for (int step = 0; step < kSteps; ++step) {
      const auto& spec = net.specs[static_cast<std::size_t>(pick(rng))];
      const auto op = want_open(rng) ? SwitchOp::Open : SwitchOp::Close;
      const auto before = mgr.states();
      auto r = mgr.actuate(spec.name, op, "prop");
      const auto after = mgr.states();

      // Closing always succeeds on ideal hardware.
      if (op == SwitchOp::Close) {
        ASSERT_TRUE(r) << "seed " << seed << " step " << step;
        continue;
      }
      if (!r) {
        ++refused;
        EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
        EXPECT_EQ(before, after) << "refused actuation changed state";
        continue;
      }
      ++accepted_opens;
      // Positive interlocks held at the moment of opening.
      for (const auto& p : spec.positive_interlocks) {
        EXPECT_TRUE(is_open(before, p)) << "seed " << seed << ": " << spec.name << " opened with " << p
                                        << " closed";
      }
      // Declared negative pairs are never open together.
      for (const auto& s : net.specs) {
        for (const auto& other : s.interlocks) {
          EXPECT_FALSE(is_open(after, s.name) && is_open(after, other))
              << "seed " << seed << " step " << step << ": " << s.name << " and " << other << " both open";
        }
      }
    }
  }
  // The generator exercised both outcomes.
  EXPECT_GT(accepted_opens, 100u);
  EXPECT_GT(refused, 100u);
}

// An open refused for interlock would have been accepted had its reason been
// absent: refusals are never spurious.
TEST(SwitchManagerProperty, RefusalsAreExplainedByDeclaredInterlocks) {
  for (int seed = 100; seed < 130; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    auto net = random_network(rng, 8);
    IdealActuator act;
    auto made = SwitchManager::create(net.specs, [&](const std::string&) -> IValveActuator* { return &act; });
    ASSERT_TRUE(made);
    auto& mgr = **made;
    ASSERT_TRUE(mgr.refresh());

    // Everything that points at a valve through a negative interlock.
    std::map<std::string, std::set<std::string>> exclusive;
    for (const auto& s : net.specs) {
      for (const auto& o : s.interlocks) {
        exclusive[s.name].insert(o);
        exclusive[o].insert(s.name);
      }
    }

    std::uniform_int_distribution<int> pick(0, 7);
    std::bernoulli_distribution want_open(0.6);
    for (int step = 0; step < 200; ++step) {
      const auto& spec = net.specs[static_cast<std::size_t>(pick(rng))];
      const auto op = want_open(rng) ? SwitchOp::Open : SwitchOp::Close;
      const auto before = mgr.states();
      auto r = mgr.actuate(spec.name, op, "prop");
      if (op == SwitchOp::Close || r) continue;
      bool explained = false;
      for (const auto& o : exclusive[spec.name]) explained |= is_open(before, o);
      for (const auto& p : spec.positive_interlocks) explained |= !is_open(before, p);
      EXPECT_TRUE(explained) << "seed " << seed << ": " << spec.name << " refused: " << r.error().what;
    }
  }
}
